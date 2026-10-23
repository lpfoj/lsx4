"""Bind post-run stderr evidence to the exact host-requested probe launch."""
import re


def _diagnostic_text(match, contents):
    diagnostic = match.group().rstrip('\r\n')
    metal_function = diagnostic.startswith('_mtlValidate')
    if not (diagnostic.startswith('-[MTLDebug') or metal_function) or diagnostic.endswith("'"):
        return diagnostic
    # Metal prints the offending resource identity on continuation lines. Keep
    # those bounded details without swallowing subsequent independent events.
    tail = contents[match.end():match.end() + 4096].lstrip('\r\n')
    for line in tail.splitlines()[:16]:
        if not (line.startswith(('<MTLToolsObject:', '    ')) or
                (metal_function and (line.startswith('source texture pixelFormat (') or line == "'"))):
            break
        if len(diagnostic) + len(line) + 1 > 4096:
            break
        diagnostic += '\n' + line
        if line.endswith("'"):
            break
    return diagnostic


def current_run_stderr(contents, expected_run_id):
    if not isinstance(expected_run_id, str) or not expected_run_id:
        return None
    # A later launch (including a malformed marker) invalidates an earlier
    # matching marker. Only diagnostics after the current marker are evidence.
    markers = list(re.finditer(r'^\[APPLE_PROBE_RUN\][^\r\n]*', contents, re.MULTILINE))
    if not markers or markers[-1].group() != '[APPLE_PROBE_RUN] id=' + expected_run_id:
        return None
    return contents[markers[-1].end():]


def inspect_stderr(contents, expected_run_id):
    current = current_run_stderr(contents, expected_run_id)
    result = {'runId': expected_run_id, 'markerMatched': current is not None,
              'fatalDiagnostic': None}
    if current is None:
        return result
    patterns = (
        r'^\[APPLE_UNHANDLED_SIGNAL\][^\r\n]*',
        r'^\[APPLE_CRITICAL\][^\r\n]*(?:Unreachable|Assertion)[^\r\n]*(?:\r?\n(?!\[)[^\r\n]*)?',
        r'^libc\+\+abi: terminating due to uncaught exception[^\r\n]*',
        r'^memory allocation of [0-9]+ bytes failed\r?$',
        # MoltenVK can lose the GPU device while CPU/audio threads keep running.
        # Ordinary recoverable Vulkan errors do not establish terminal failure.
        r'^\[mvk-error\] VK_ERROR_[A-Z_]+: Lost VkDevice [^\r\n]*',
        r'^-\[MTLDebug[A-Za-z0-9_]+ [^\r\n]+\]:[0-9]+: failed assertion [^\r\n]*',
        r'^_mtlValidate[A-Za-z0-9_]+:[0-9]+: failed assertion [^\r\n]*',
        # A failed FEX worker can leave the presenter flipping an empty frame.
        # The native bridge rejects either API status or callback result status.
        # Match the leading fields so thread names cannot invent a failure.
        # A captured signal or clean worker exit alone is not terminal evidence.
        r'^(?:\[LSX4Native\] )?\[EXECUTOR_GUEST_THREAD_FEX\] '
        r'(?:status=-[1-9][0-9]*\b|status=-?[0-9]+\s+result=-[1-9][0-9]*\b)[^\r\n]*',
    )
    fatal = [match for pattern in patterns
             for match in re.finditer(pattern, current, re.MULTILINE)]
    if fatal:
        # A fatal handler may deliberately trap while terminating. Preserve the
        # first observed diagnostic instead of replacing its fault address with
        # that secondary SIGTRAP. Keep the complete chronological chain when
        # multiple diagnostics exist; ordering alone does not prove causality.
        diagnostics = [_diagnostic_text(match, current)
                       for match in sorted(fatal, key=lambda match: match.start())]
        result['fatalDiagnostic'] = diagnostics[0]
        if len(diagnostics) > 1:
            result['fatalDiagnostics'] = diagnostics
    return result


def can_continue_after_transport_failure(awake_recovered, returned, stderr_evidence):
    # A returned report has its separate existing launch-timestamp gate. A
    # stderr-only crash requires the UUID marker; legacy logs cannot prove it.
    return bool(awake_recovered and (returned or (
        stderr_evidence.get('markerMatched') and stderr_evidence.get('fatalDiagnostic'))))


def confirmed_awake_recovery(recovery, expected_run_id):
    return bool(expected_run_id and recovery.get('runId') == expected_run_id
                and recovery.get('attempted') is True
                and recovery.get('awakeRecovered') is True)

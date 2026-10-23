"""Finite host-side deadlines for debugger preparation and device evidence."""


def debugger_ready_timeout(seconds, prepared_pool_mib=None):
    if not 5 <= seconds <= 600:
        raise ValueError("Observation duration must be 5–600 seconds")
    if prepared_pool_mib is None:
        return seconds + 90
    if not 4 <= prepared_pool_mib <= 512 or prepared_pool_mib % 4:
        raise ValueError("Prepared pool must be 4–512 MiB in 4 MiB chunks")
    # Observed Wi-Fi preparation takes ~2.6 seconds per 4 MiB callback,
    # excluding the surrounding debugger stop/resume traffic. Keep a finite
    # margin; progress messages never extend this deadline. The game timer
    # starts only after the prepared-pool breakpoint and debugger detach.
    return 90 + 5 * (prepared_pool_mib // 4)


def device_probe_timeout(seconds, prepared_pool_mib=None):
    # Outside the inner ready deadline: observation/report grace, then a
    # bounded allowance for attach, profile/input transfer, LLDB commands and
    # final awake recovery/evidence copies. Individual operations still have
    # their own shorter limits. This is recalculated for every pool retry.
    return debugger_ready_timeout(seconds, prepared_pool_mib) + seconds + 30 + 360

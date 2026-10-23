// SPDX-License-Identifier: GPL-2.0-or-later
// Fixed LSX4 protocol for the on-device helper. No user-provided scripts.
// Only existing fresh RX slabs are touched; the app owns allocation/aliasing.
function register(reply, index) {
    const match = new RegExp('(?:^T[0-9a-fA-F]{2}|;)' + index + ':([0-9a-fA-F]{16});').exec(reply);
    if (!match) throw new Error('Missing register ' + index);
    const bytes = match[1].match(/../g);
    return BigInt('0x' + bytes.reverse().join(''));
}
function little(value) {
    let output = '';
    for (let i = 0; i < 8; ++i) {
        output += Number(value & 255n).toString(16).padStart(2, '0');
        value >>= 8n;
    }
    return output;
}
function okay(command) {
    const response = send_command(command);
    if (response !== 'OK') throw new Error('JIT operation rejected: ' + command.slice(0, 28));
}
const pid = get_pid();
if (!Number.isInteger(pid) || pid <= 0) throw new Error('Invalid target process');
const attached = send_command('vAttach;' + pid.toString(16));
if (!/^T[0-9a-fA-F]{2}/.test(attached)) throw new Error('JIT attach failed');
let prepared = 0;
let detached = false;
while (!detached) {
    const stop = send_command('c');
    if (!/^T05/.test(stop)) throw new Error('Unexpected target signal during JIT preparation');
    const thread = /(?:^T[0-9a-fA-F]{2}|;)thread:([0-9a-fA-F]+);/.exec(stop);
    if (!thread) throw new Error('Missing stopped thread');
    const tid = thread[1];
    const pc = register(stop, '20');
    const command = register(stop, '10');
    const instruction = send_command('m' + pc.toString(16) + ',4').toLowerCase();
    if (instruction !== 'a0013ed4') throw new Error('Unexpected JIT breakpoint instruction');
    if (command === 1n) {
        const address = register(stop, '00');
        const bytes = register(stop, '01');
        if (address === 0n || address % 16384n !== 0n || bytes <= 0n
                || bytes > 4194304n || bytes % 16384n !== 0n
                || address + bytes > 9007199254740991n) {
            throw new Error('Invalid fresh JIT slab');
        }
        // Native helper: exact one-byte zero write per 16 KiB page, pipelined
        // in batches. Its implementation verifies every debugserver response.
        if (prepare_memory_region(Number(address), Number(bytes)) !== 'OK') {
            throw new Error('Page preparation failed');
        }
        // Publish the existing runtime acknowledgement only after ALL pages.
        okay('M' + address.toString(16) + ',8:4c5358344a495421');
        if (send_command('m' + address.toString(16) + ',8') !== '4c5358344a495421') {
            throw new Error('JIT acknowledgement readback failed');
        }
        okay('P20=' + little(pc + 4n) + ';thread:' + tid + ';');
        prepared += Number(bytes);
        log('LSX4_LOCAL_JIT_PREPARED_BYTES ' + prepared);
    } else if (command === 0n) {
        if (!prepared) throw new Error('Detach without a prepared pool');
        okay('P20=' + little(pc + 4n) + ';thread:' + tid + ';');
        okay('D');
        detached = true;
        log('LSX4_LOCAL_JIT_DETACHED');
    } else {
        throw new Error('Unknown LSX4 JIT command');
    }
}

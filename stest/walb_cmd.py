import collections
import os
import subprocess
import sys
import time
import socket
import errno

TIMEOUT_SEC = 1000

Server = collections.namedtuple('Server', 'name port vg')
Config = collections.namedtuple('Config', 'debug binDir dataDir storageL proxyL archiveL')

cfg = None

def set_config(config):
    if config.binDir[0] != '/' or config.binDir[-1] != '/':
        raise Exception("binDir must abs path", config.binDir)
    if config.dataDir[0] != '/' or config.dataDir[-1] != '/':
        raise Exception("dataDir must abs path", config.dataDir)
    global cfg
    cfg = config

def make_dir(pathStr):
    if not os.path.exists(pathStr):
        os.makedirs(pathStr)

def to_str(ss):
    return " ".join(ss)

def get_debug_opt():
    if cfg.debug:
        return ["-debug"]
    else:
        return []

def get_host_port(s):
    return "localhost" + ":" + s.port

def get_server_args(server):
    if server in cfg.storageL:
        ret = [cfg.binDir + "storage-server",
               "-archive", get_host_port(cfg.archiveL[0]),
               "-proxy", ",".join(map(get_host_port, cfg.proxyL))]

    elif server in cfg.proxyL:
        ret = [cfg.binDir + "proxy-server"]

    elif server in cfg.archiveL:
        ret = [cfg.binDir + "archive-server", "-vg", server.vg]
    else:
        raise Exception("Server name %s is not found in all lists" % server.name)
    ret += ["-p", server.port,
            "-b", cfg.dataDir + server.name,
            "-l", server.name + ".log",
            "-id", server.name] + get_debug_opt()
    return ret

def run_command(args):
    if cfg.debug:
        print "run_command:", to_str(args)
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=sys.stderr)
    f = p.stdout
    s = f.read().strip()
    ret = p.wait()
    if ret != 0:
        raise Exception("command error %d\n" % ret)
    return s

def run_ctl(server, cmdArgs):
    ctlArgs = [cfg.binDir + "/controller",
            "-id", "ctrl",
            "-a", "localhost",
            "-p", server.port] + get_debug_opt()
    return run_command(ctlArgs + cmdArgs)

def run_daemon(args):
    try:
        pid = os.fork()
        if pid > 0:
            return
    except OSError, e:
        print >>sys.stderr, "fork#1 failed (%d) (%s)" % (e.errno, e.strerror)

    os.chdir("/")
    os.setsid()
    os.umask(0)

    sys.stdin = open('/dev/null', 'r')
    sys.stdout = open('/dev/null', 'w')
    sys.stderr = open('/dev/null', 'w')

    subprocess.Popen(args).wait()
    sys.exit(0)

def wait_for_server_port(server):
    address = "localhost"
    port = int(server.port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(1.0)
    for i in range(1, 10):
        try:
            sock.connect((address, port))
            sock.close()
            return
        except socket.error, e:
            if e.errno not in [errno.ECONNREFUSED, errno.ECONNABORTED]:
                raise
        time.sleep(0.1)

#def hostType(server):
#    return run_ctl(server, ["host-type"])

def get_state(server, vol):
    return run_ctl(server, ["get-state", vol])

def set_slave_storage(sx, vol):
    state = get_state(sx, vol)
    if state == 'Slave':
        return
    if state == 'Master' or state == 'WlogSend':
        stop(sx, vol, 'Stopped')
    else:
        raise Exception('set_slave_storage:bad state', state)
    stop_sync(cfg.archiveL[0], vol)
    run_ctl(sx, ["reset-vol", vol])
    run_ctl(sx, ["start", vol, "slave"])
    wait_for_state(sx, vol, "Slave")

##################################################################
# user command functions

def wait_for_state(server, vol, state, timeoutS = 10):
    for c in xrange(0, timeoutS):
        st = get_state(server, vol)
        print "c=", server, vol, state, c, st
        if st == state:
            return
        time.sleep(1)
    raise Exception("wait_for_state", server, vol, state)

def kill_all_servers():
    for s in ["storage-server", "proxy-server", "archive-server"]:
        subprocess.Popen(["/usr/bin/killall", "-9"] + [s]).wait()
    time.sleep(1)

def startup(server):
    make_dir(cfg.dataDir + server.name)
    args = get_server_args(server)
    if cfg.debug:
        print 'cmd=', args
    run_daemon(args)
    wait_for_server_port(server)

def startup_all():
    for s in cfg.archiveL + cfg.proxyL + cfg.storageL:
        startup(s)

def shutdown(server, mode="graceful"):
    run_ctl(server, ["shutdown", mode])

def shutdown_all():
    for s in cfg.storageL + cfg.proxyL + cfg.archiveL:
        shutdown(s)

def init(sx, vol, wdevPath):
    run_ctl(sx, ["init-vol", vol, wdevPath])
    run_ctl(sx, ["start", vol, "slave"])
    wait_for_state(sx, vol, "Slave")
    run_ctl(cfg.archiveL[0], ["init-vol", vol])

def is_synchronizing(ax, vol):
    px = cfg.proxyL[0]
    st = run_ctl(px, ["archive-info", "list", vol])
    return ax in st.split()

# stop s vol and wait until state is waitState
def stop(s, vol, waitState, mode = "graceful"):
    run_ctl(s, ["stop", vol, mode])
    wait_for_state(s, vol, waitState)

def start(s, vol, waitState):
    run_ctl(s, ["start", vol])
    wait_for_state(s, vol, waitState)

def stop_sync(ax, vol):
    for px in cfg.proxyL:
        stop(px, vol, "Stopped")
        run_ctl(px, ["archive-info", "delete", vol, ax.name])
        state = get_state(px, vol)
        if state == 'Stopped':
            start(px, vol, "Started")

def get_gid_list(ax, vol, cmd):
    if not cmd in ['list-restorable', 'list-restored']:
        raise Exception('get_list_gid : bad cmd', cmd)
    ret = run_ctl(ax, [cmd, vol])
    return map(int, ret.split())

def list_restorable(ax, vol):
    ret = run_ctl(ax, ["list-restorable", vol])
    return map(int, ret.split())

def list_restored(ax, vol):
    ret = run_ctl(ax, ["list-restored", vol])
    return map(int, ret.split())

def wait_for_restorable_any(ax, vol, timeoutS = 0x7ffffff):
    for c in xrange(0, timeoutS):
        gids = get_gid_list(ax, vol, 'list-restorable')
        if gids:
            return gids[-1]
        time.sleep(1)
    return -1

def wait_for_gid(ax, vol, gid, cmd, timeoutS = 0x7ffffff):
    for c in xrange(0, timeoutS):
        gids = get_gid_list(ax, vol, cmd)
        if gid in gids:
            return True
        time.sleep(1)
    return False

def wait_for_not_gid(ax, vol, gid, cmd, timeoutS = 0x7ffffff):
    for c in xrange(0, timeoutS):
        gids = get_gid_list(ax, vol, cmd)
        if gid not in gids:
            return True
        time.sleep(1)
    return False

def wait_for_restorable(ax, vol, gid, timeoutS = 0x7ffffff):
    return wait_for_gid(ax, vol, gid, 'list-restorable', timeoutS)

def wait_for_restored(ax, vol, gid, timeoutS = 0x7fffffff):
    return wait_for_gid(ax, vol, gid, 'list-restored', timeoutS)

def wait_for_not_restored(ax, vol, gid, timeoutS = 0x7fffffff):
    return wait_for_not_gid(ax, vol, gid, 'list-restored', timeoutS)

def add_archive_to_proxy(px, vol, ax):
    st = get_state(px, vol)
    if st == "Started":
        stop(px, vol, "Stopped")
    run_ctl(px, ["archive-info", "add", vol, ax.name, get_host_port(ax)])
    start(px, vol, "Started")

def prepare_backup(sx, vol):
    a0 = cfg.archiveL[0]
    st = get_state(sx, vol)
    if st == "Slave":
        stop(sx, vol, "SyncReady")

    ret = run_ctl(sx, ["is-overflow", vol])
    if ret != "0":
        run_ctl(sx, ["reset-vol", vol])

    for s in cfg.storageL:
        if s == sx:
            continue
        st = get_state(s, vol)
        if st != "Slave" and st != "Clear":
            raise Exception("full_backup : bad state", s.name, vol, st)

    for ax in cfg.archiveL[1:]:
        if is_synchronizing(ax, vol):
            stop_sync(ax, vol)

    for px in cfg.proxyL:
        add_archive_to_proxy(px, vol, a0)

def full_backup(sx, vol):
    a0 = cfg.archiveL[0]
    prepare_backup(sx, vol)
    run_ctl(sx, ["full-bkp", vol])
    wait_for_state(a0, vol, "Archived", TIMEOUT_SEC)

    for c in xrange(0, TIMEOUT_SEC):
        gids = get_gid_list(a0, vol, 'list-restorable')
        if gids:
            return gids[-1]
        time.sleep(1)
    raise Exception('full_backup:timeout', sx, vol)

def hash_backup(sx, vol):
    a0 = cfg.archiveL[0]
    prepare_backup(sx, vol)
    prev_gids = get_gid_list(a0, vol, 'list-restorable')
    if prev_gids:
        max_gid = prev_gids[-1]
    else:
        max_gid = -1

    run_ctl(sx, ["hash-bkp", vol])
    wait_for_state(a0, vol, "Archived", TIMEOUT_SEC)

    for c in xrange(0, TIMEOUT_SEC):
        gids = get_gid_list(a0, vol, 'list-restorable')
        if gids and gids[-1] > max_gid:
            return gids[-1]
        time.sleep(1)
    raise Exception('hash_backup:timeout', sx, vol)

def write_random(devName, size):
    args = [cfg.binDir + "/write_random_data",
        '-s', str(size), devName]
    return run_command(args)

def get_sha1(devName):
    ret = run_command(['/usr/bin/sha1sum', devName])
    return ret.split(' ')[0]

def restore(ax, vol, gid):
    run_ctl(ax, ['restore', vol, str(gid)])
    wait_for_restored(ax, vol, gid)

def del_restored(ax, vol, gid):
    run_ctl(ax, ['del-restored', vol, str(gid)])
    wait_for_not_restored(ax, vol, gid)

def get_restored_path(ax, vol, gid):
    return '/dev/' + ax.vg + '/r_' + vol + '_' + str(gid)

def snapshot_async(sx, vol):
    state = get_state(sx, vol)
    if state != 'Master' and state != 'WlogSend':
        raise Exception('snapshot_async', state)
    gid = run_ctl(sx, ['snapshot', vol])
    return int(gid)

def snapshot_sync(sx, vol, axs):
    gid = snapshot_async(sx, vol)
    for ax in axs:
        wait_for_restorable(ax, vol, gid)
    return gid

def verify_equal_sha1(msg, md0, md1):
    if md0 == md1:
        print msg + ' ok'
    else:
        raise Exception('fail ' + msg, md0, md1)

def restore_and_verify_sha1(msg, md0, ax, vol, gid):
    restore(ax, vol, gid)
    restoredPath = get_restored_path(ax, vol, gid)
    print "restoredPath=", restoredPath
    md1 = get_sha1(restoredPath)
    verify_equal_sha1(msg, md0, md1)
    del_restored(ax, vol, gid)

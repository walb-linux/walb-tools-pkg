import os
from walb_cmd import *

s0 = Server('s0', '10000', None)
s1 = Server('s1', '10001', None)
s2 = Server('s2', '10002', None)
p0 = Server('p0', '10100', None)
p1 = Server('p1', '10101', None)
p2 = Server('p2', '10102', None)
a0 = Server('a0', '10200', 'vg0')
a1 = Server('a1', '10201', 'vg1')
#a2 = Server('a2', '10202', None)

WORK_DIR = os.getcwd() + '/stest/tmp/'

config = Config(True, os.getcwd() + '/binsrc/', WORK_DIR, [s0, s1], [p0, p1], [a0, a1])

WDEV_PATH = '/dev/walb/0'
VOL = 'vol0'

set_config(config)

def setup_test():
    run_command(['/bin/rm', '-rf', WORK_DIR])
    if os.path.isdir('/dev/vg0'):
        for f in os.listdir('/dev/vg0'):
            if f[0] == 'i':
                run_command(['/sbin/lvremove', '-f', '/dev/vg0/' + f])
    make_dir(WORK_DIR)
    kill_all_servers()
    startup_all()


def test_n1():
    """
        full-backup -> sha1 -> restore -> sha1
    """
    init(s0, VOL, WDEV_PATH)
    write_random(WDEV_PATH, 1)
    md0 = get_sha1(WDEV_PATH)
    gid = full_backup(s0, VOL)
    print "gid=", gid
    restore_and_verify_sha1('test_n1', md0, a0, VOL, gid)

def test_n2():
    """
        write -> sha1 -> snapshot -> restore -> sha1
    """
    write_random(WDEV_PATH, 1)
    md0 = get_sha1(WDEV_PATH)
    gid = snapshot_sync(s0, VOL, [a0])
    print "gid=", gid
    print list_restorable(a0, VOL)
    restore_and_verify_sha1('test_n2', md0, a0, VOL, gid)

def test_n3():
    """
        hash-backup -> sha1 -> restore -> sha1
    """
    set_slave_storage(s0, VOL)
    write_random(WDEV_PATH, 1)
    md0 = get_sha1(WDEV_PATH)
    gid = hash_backup(s0, VOL)
    print "gid=", gid
    restore_and_verify_sha1('test_n3', md0, a0, VOL, gid)

def main():
    setup_test()
    test_n1()
    test_n2()
    test_n3()

if __name__ == "__main__":
    main()

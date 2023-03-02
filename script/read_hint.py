import os
import _thread
from multiprocessing import Process
import time
import subprocess
from signal import SIGTERM
from signal import SIGKILL
from signal import SIGINT


def func(i):
    comm = "bash slice.sh {0}".format(i)
    p = subprocess.Popen(comm,shell=True)

i = 1
pid = ""
while True:
# while i <= 1 :
    # print("===================第{0}次运行=================\n".format(i))
    # i += 1
    with open('/home/snow/PGO-CAT/profiling/build/tmp/myTest.txt', mode='r+') as f:
        r = f.readlines()
        hint = r[0].strip('\n')
        if hint != '1':
            f.seek(0,0)
            print("find hint\n")
            f.write('1\n')
            
            # getpid = os.popen("ps -ef | grep -E \"toplev|slice.sh\" | grep -v 'grep' | awk '{print $2}'")
            getpid = os.popen("ps -ef | grep slice.sh | grep -v 'grep' | awk '{print $2}'")
            pid = getpid.read().strip('\n').replace("\n", " ")
            getpid.close()
            print("pid=", pid)
            if pid == "":
                print("no pmu process")
            else:
                print(pid)
                command = "sudo kill -9 {0}".format(pid)
                print(command)
                p = subprocess.run("exec " + command,shell=True,preexec_fn=os.setsid)
                # p.kill()
                # os.killpg(3553967,SIGINT)
            # p = Process(target=func,args=(i, ))
            # p.start()
            comm = "bash slice.sh {0}".format(i)
            p = subprocess.Popen(comm,shell=True)
            i += 1
    # else:
        # time.sleep(1)

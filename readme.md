## 使用流程
https://github.com/intel/intel-cmt-cat/wiki/Usage-Examples # cmt使用方法教程


## 安装cat
apt install intel-cmt-cat

## 查看是否支持cat
sudo pqos -s
## 结果需要出现L2CA
如果没有出现，使用sudo pqos --iface=msr -s


## 修改cat
pqos -e "llc:1=0x000f;llc:2=0x0ff0;"
或者使用 sudo pqos --iface=msr -e "llc:1=0x000f;llc:2=0x0ff0;"


## 绑定进程和COS
sudo pqos -I -a "pid:1=33204;"

## 运行pebs代码
gcc test.c -o test.o
sudo ./test.o $pid

## 整体流程
先运行ranacc（被测试程序），获取该程序pid
设置cat，绑定pid至指定COS
运行pebs程序，继续运行ranacc程序
#利用cin使得程序卡住

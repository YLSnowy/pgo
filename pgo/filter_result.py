import matplotlib.pyplot as plt
import os
os.environ['DISPLAY'] = ':0'



f = open("a.txt")
# lines = f.read()
result = []
for line in f:
    # print(line.split())
    miss = (float)(line.split(' ')[3])
    access = (float)(line.split(' ')[-1])
    if access != 0:
        miss_ratio = miss / access
    else:
        miss_ratio = 0
    result.append(miss_ratio)
x_1 = [i for i in range(6985,7000)]
x_2 = [i for i in range(10484, 10525)]
x = x_1 + x_2
x = [i for i in range(len(x))]
y_1 = [result[i] for i in range(6985, 7000)]
y_2 = [result[i] for i in range(10484, 10525)]
y = y_1 + y_2
plt.scatter(x, y)
plt.savefig('./result.png')
# plt.show()



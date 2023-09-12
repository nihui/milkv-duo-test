
# 编译指北

1. 修改 `CMakeLists.txt` 有关 duo sdk 路径
2. 设置toolchain变量，cmake编译
```
export RISCV_ROOT_PATH=/home/nihui/osd/host-tools/gcc/riscv64-linux-musl-x86_64

mkdir build
cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../riscv64-unknown-linux-musl.toolchain.cmake ..
make
$RISCV_ROOT_PATH/bin/riscv64-unknown-linux-musl-strip testmilkv
make install
```
3. 由于rpath原因，需要 install 后把 install/bin/testmilkv 拷贝到 milkv-duo 上运行，否则会找不到 libvdec.so 等等...


# 程序主要流程

1. 读图片
2. 解析文件头获取 w h pixel_format 等
3. 初始化 vbpool
4. 初始化 vdec vpss旋转 vpss转rgb
5. 发送jpg数据，vdec拿到yuv
6. 发送yuv给vpss旋转，拿到旋转后的nv12
7. 发送nv12给vpss转rgb，拿到rgb
8. map，拷贝rgb，unmap
9. 清理frame
10. 清理 vpss vdec vbpool
11. stb存出结果图像

# 发现的问题

1. 1920x1080 90/270旋转图像有区域内容错误

<img src="https://github.com/nihui/milkv-duo-test/blob/master/out/1920x1080.jpg.5.jpg?raw=true" width="400">
<img src="https://github.com/nihui/milkv-duo-test/blob/master/out/1920x1080.jpg.8.jpg?raw=true" width="400">

2. 很多分辨率的图片解码失败，尤其是 milkv-duo 运行时间长之后或多次失败，再次运行导致失败率大幅上升，主要错误在 vpss旋转/转rgb c006800e c0068003 两种

```
[root@milkv]~# ./testmilkv
stFrameInfo 1   120 x 90  13
CVI_VPSS_GetChnFrame rgb failed c006800e
decode failed 120x90.jpg 1

stFrameInfo 1   160 x 120  13
CVI_VPSS_SendFrame rgb failed c0068003
decode failed 160x120.jpg 1

stFrameInfo 1   320 x 240  13
CVI_VPSS_SendFrame rgb failed c0068003
decode failed 320x240.jpg 1

stFrameInfo 1   400 x 300  13
CVI_VPSS_SendFrame rgb failed c0068003
decode failed 400x300.jpg 1

stFrameInfo 1   800 x 600  13
CVI_VPSS_SendFrame rgb failed c0068003
decode failed 800x600.jpg 1

stFrameInfo 1   800 x 800  13
CVI_VPSS_SendFrame rgb failed c0068003
decode failed 800x800.jpg 1

stFrameInfo 1   960 x 540  13
CVI_VPSS_SendFrame rgb failed c0068003
decode failed 960x540.jpg 1

stFrameInfo 1   1080 x 1080  13
CVI_VPSS_SendFrame rgb failed c0068003
decode failed 1080x1080.jpg 1
```

3. CVI_SYS_Bind 将vdec和vpss自动串起来不起作用，CVI_VPSS_GetChnFrame 超时失败

4. vpss 似乎总是会用 common vb pool，即便我已经 attach 自己的 vbpool，common 不够大依然会失败

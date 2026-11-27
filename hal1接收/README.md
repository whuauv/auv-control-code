# GCC工程外壳使用说明
本项目依托 HAL库 开发，支持CubeMX + Keil（或GCC）环境，二者可并行不悖。  

### GCC编译Keil工程的最小依赖
在 Keil工程根目录下放置
**Makefile、startup_stm32f103xb.s、STM32F103xx_FLASH.ld**，
这三个文件分别是**编译说明书、芯片启动文件和编译链接文件**。
这是专门配给 GCC 用的，**缺一不可**。同时，不会影响 Keil工程自己的文件。  

使用 VS code 作编译器的开发者，还应在根目录下建立一个.vscode目录，里面放 c_cpp_properties.json、launch.json、tasks.json，这三个文件分别用于VS Code 的 IntelliSense 代码检查、Cortex-Debug 扩展调试配置、托管运行。

Makefile 文件里定义了工程名、源代码等信息 以及编译器、烧录器等命令，所以用make可以根据这个文件里的规则调用这些工具和代码执行功能。
本README.MD说明书作者的设备上配置的是来自ARM官网的 arm-none-eabi-gcc编译器（以下简称gcc或armgcc，以区别与x86下的gcc）、来自 Github 的
openocd烧录工具、来自MingW64的C语言编译环境自带的mingw32-make工具（以下简称make）以及STLink驱动器等。  
mingw32-make用gcc编译时根据Makefile自动在根目录下建立 build/ 目录，并把编译产物放置其下。


# Makefile语法简介
Makefile本质上就是一份给make工具看的编译说明书，是为了方便开发者敲命令行而设计的（否则手敲命令行就得一个一个文件地编译、链接、最后烧录），于是本着程式化复用命令的原则，Makefile 语法中最基础的就是变量赋值。

## 变量赋值
和定义宏类似，变量赋值后可以在下面的构建命令中以 ` $(变量) ` 形式反复使用 ，make读取时自动展开。

常见赋值符号：  
` = ` 普通赋值，引用时再展开。  
` := ` 立即展开赋值，定义这一行时就计算右侧内容。  
` += ` 追加内容。  
` ?= ` 只有变量还没有值时才赋值。  

【注】注释添加在赋值语句行后时，不要空格！这与Python的PEP8编码规范不同。否则会导致编译错误：  
```Makefile
BUILD    := build  # 编译产物目录
```
因为 Makefile 赋值行末尾的 # 只剥掉 # 及后面的文字，这样写相当于把 build 以及其后的两个空格也赋值给了 BUILD


#### wildcard 函数，用来按文件名模式查找文件。
`$(wildcard Core/Src/*.c)` 意思是：找出 Core/Src 文件夹下所有扩展名为 .c 的文件。  

```Makefile
C_SOURCES = $(wildcard Core/Src/*.c) \
            $(wildcard Drivers/STM32F1xx_HAL_Driver/Src/*.c)
```
这样用通配符自动收集这些目录里的 C 源文件，并赋值给 C_SOURCES ，避免在 Makefile 里逐个列出文件名。  
#### addprefix 函数：给每个文件名加上前缀
#### notdir 函数：只保留文件名，去掉目录部分
```Makefile
OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
```
这样就实现了从源文件路径到编译产物路径的映射，比如使 Core/Src/main.c 最终会对应到 build/main.o


#### 编译器和工具

```Makefile
PREFIX = arm-none-eabi-
CC = $(PREFIX)gcc#C 编译器
AS = $(PREFIX)gcc -x assembler-with-cpp#汇编工具
CP = $(PREFIX)objcopy#用于把 ELF 转换成 HEX 或 BIN
SZ = $(PREFIX)size#显示程序大小
```
指定编译器名称和语法，编译参数：使用 Cortex-M3 内核，并生成 Thumb 指令
```Makefile
CPU = -mcpu=cortex-m3
MCU = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)
```



#### Openocd烧录命令

```powershell
openocd  -f interface/stlink.cfg  -f target/stm32f1x.cfg  -c "program ..."
              ↑                          ↑
         「调试器那一端」            「芯片那一端」
```

Keil 的 Device 下拉框「选 STM32F103C8」同时决定了本来不相干的两件事:  
   ├── 编译侧：Flash 64K 从 0x08000000、RAM 20K 从 0x20000000  
   └── 烧录侧：用哪个 Flash 算法、页大小多少  

OpenOCD 把它拆开，各自归各自的文件：  
编译侧  →  STM32F103xx_FLASH.ld（根目录那个，写明 FLASH 64K / RAM 20K / _estack）
烧录侧  →  target/stm32f1x.cfg（flash bank ... stm32f1x ... 靠运行时探测容量）  

## Makefile规则：目标、依赖和命令
make指令格式`make [-f filename] [names]`，其中`filename`代表make所用的项目描述文件makefile，如果此项省略，则从当前目录下按GNUmakefile、makefile、Makefile的顺序寻找默认的项目描述文件。  

在终端敲make命令，就是在调用make工具（通常是写入环境变量的make.exe）按照项目描述文件来执行命令。Makefile 以这样的格式来写清目标、依赖和命令：  
```text
目标: 依赖项
<Tab>生成目标的命令
```
make 命令必须处理至少一个目标，在 make 命令后指定目标名，可以让 make 按照对应目标下的内容执行命令；如不指定目标名，make 就会自动找到第一个目标执行并退出  
【例1】当一个目标依赖其他目标或文件时，make 先执行依赖的目标或按相关规则更新依赖的文件。
```makefile
one:two
	@echo one!
two:
	@echo two!
```
【例2】要生成 build/hal1tx.elf 编译文件，需要所有目标文件和 Makefile；如果需要生成，就运行 GCC 链接命令。
```Makefile
$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
    $(CC) $(OBJECTS) $(LDFLAGS) -o $@
```


常用自动变量：  
` $@ `：当前规则的目标文件名。  
` $< `：第一个依赖项。  
` $^ `：所有依赖项。  
% 是模式匹配符，可以代表相同的文件名部分。  

```Makefile
$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
    $(HEX) $< $@
```
假如要生成 build/hal1tx.hex，Make 会用 build/hal1tx.elf 作为 $<，用 build/hal1tx.hex 作为 $@ 。竖线 | 表示后面的目录是顺序依赖  



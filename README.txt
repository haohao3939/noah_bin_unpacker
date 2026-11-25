诺亚舟词典 .bin 解包工具 (支持 dict V0.2 与 DictAdd V1.0)
===========================================================

【随开即用】
1. 方式一（拖拽）：直接把 .bin 文件拖到 一键解包.bat 上松开即可。
2. 方式二（同目录双击）：把 .bin 文件和本工具放在同一个文件夹里，双击 一键解包.bat 自动识别并解包。
3. 方式三（命令行）：
   noah_dict_unpack.exe <输入.bin文件路径> [输出文件夹路径]
   （省略输出路径时，默认在程序所在目录生成 "<文件名>_extracted" 文件夹）

【输出内容说明】
1) 动漫词典类 (dict V0.2)：
   - images\             解出的全部 JPEG 图片（img_0000.jpg ...）
   - audio\              解出的全部 MP3 音频（audio_00.mp3 ...）
   - animation_index.tsv 词条与动画帧图片映射表（UTF-8 BOM）
   - dictionary.txt      解码后的词典文本（UTF-8 BOM，记事本可直接打开）

2) 诺亚词霸扩展库类 (DictAdd V1.0)：
   - all_entries.txt     按词条汇总的全部扩展知识文本（UTF-8 BOM）
   - categories\         按栏目拆分导出的独立文本文件（如 01_传说、神话.txt 等）

【源码与重新编译】
- src\noah_dict_unpack.c   完整 C 语言源代码（不依赖第三方库，仅依赖系统 msvcrt.dll / kernel32.dll）
- src\build.bat            使用 MinGW gcc 重新编译的批处理脚本
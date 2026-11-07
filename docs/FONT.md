# 中文字体（GB2312 子集，18px，4bpp）

固件里的中文字体不是 LVGL 自带的（`simsun_16_cjk` 只有 ~1200 字，`摄`/`头` 都没有），
而是用 [`tools/gen_cjk_font.py`](../tools/gen_cjk_font.py) 从系统 TTF 栅格化生成的
LVGL 9 C 数组字体：

| 项目 | 值 |
|---|---|
| 文件 | `main/fonts/lv_font_noto_18_cjk.c`（约 7.8MB 源码，1.1MB 位图） |
| 字库 | **Noto Sans SC**（SIL Open Font License 1.1） |
| 覆盖 | ASCII + GB2312 全集（6763 汉字 + 682 符号）+ 常用标点，共 7542 字 |
| 参数 | 18px，4bpp，反走样，`LV_FONT_FMT_TXT_CMAP_SPARSE_FULL` |

## 重新生成

```powershell
# 需要 Pillow（pip install pillow）
python tools/gen_cjk_font.py --size 18 --font C:/Windows/Fonts/NotoSansSC-VF.ttf --name lv_font_noto_18_cjk
```

换字号或换字体都可以，改完记得同步 `main/CMakeLists.txt` 里的文件名和
`main/ui_home.c` 里的 `lv_font_noto_18_cjk` 符号。

## 许可提示

- **Noto Sans SC** 是 SIL OFL 1.1，允许嵌入、修改、随作品分发，
  但需要随作品保留版权声明与许可文本（见 [OFL-1.1](https://openfontlicense.org/)）。
- 早期版本曾用 Windows 自带的 **Microsoft YaHei** 生成，该字体**不可再分发**，
  已替换成 Noto；如果你 fork 了旧版本，请重新生成字体。
- 生成的是**位图**（每个字形的像素），不是字体文件本身；仍建议在 README / LICENSE 里
  标注字体来源与 OFL。

## 变量字体注意（踩过的坑）

`NotoSansSC-VF.ttf` 是**变量字体**，默认实例是 **Thin（wght=100）**，直接栅格化会让
英文/数字的笔画细到在 4bpp 下几乎消失，屏幕上看就像乱码（中文笔画多反而正常）。
生成脚本默认会用 `set_variation_by_name("Regular")` 切到 Regular：

```powershell
python tools/gen_cjk_font.py --size 18 --font C:/Windows/Fonts/NotoSansSC-VF.ttf --variation Regular --name lv_font_noto_18_cjk
```

如果换成静态字体（无 variation），脚本会打印 "variation not applied" 并继续，不影响生成。

# 取 dyld shared cache（iOS 16/17）

## 结论先行

cache **不在** Restore IPSW 的 OS dmg 里，而在 **SystemOS（cryptex）dmg** 的：

```
System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64*
```

以 iPad7,1 / iPadOS 17.7.11（21H461）为例：54 个文件——主文件 `dyld_shared_cache_arm64`
加 `.01`…`.52` 分卷（含 `.NN.dylddata` / `.NN.dyldlinkedit`）和 `.symbols`，合计约 3.99 GB。

**注意架构名是 `arm64`，不是 `arm64e`。** 按 `*arm64e*` 去搜会一无所获。

## 两个坑（都实测踩过）

1. **`7z l <dmg> '*dyld*'` 会假阴性。** 这版 7-Zip（23.01）对 APFS dmg 带 pattern 的列表
   返回 `0 files`，但**完整列目录**（`7z l <dmg> | grep com.apple.dyld`）能看到全部 54 个文件。
   不要拿通配符过滤的结果下"没有"的结论。
2. **`ipsw extract --dyld` 需要 apfs-fuse。** Apple Wiki 给的标准命令在 iOS 17 上会走到
   `Preparing caches: SystemOS` 然后报 `failed to find apfs-fuse`。装上以后也不一定能成：
   iOS 16+ 的 cryptex 用了新压缩，Linux 侧 apfs-fuse 目前解不开（apfs-fuse issue #168 仍是打开状态）。
   所以**直接用 7z 完整列目录 + 提取**更省事。

## 复现命令

```sh
# 1) 从 IPSW 里取出 SystemOS dmg（第二个 dmg，约 3.3 GB）
7z e iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw 044-76034-038.dmg

# 2) 完整列目录，确认 cache 存在（关键：不要加 pattern）
7z l 044-76034-038.dmg | grep 'com.apple.dyld/'

# 3) 提取（这里的通配符是生效的，不要用跨目录的 '*dyld*'）
7z e 044-76034-038.dmg -o./dsc 'System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64*' -y
```

提取时会多出 6 个 `…:com.apple.ResourceFork` 伴生文件（APFS 扩展属性），不是 cache 数据，忽略。

## 为什么"最大 dmg 里就有"的老教程失效了

iOS 15.6 之前，cache 在 OS dmg 的 `/System/Library/Caches/com.apple.dyld/`；
之后移进 cryptex（SystemOS）。实测这个 build 的 OS dmg 里连 `usr/lib/libSystem.dylib`
都没有实体文件（`usr/lib/*.dylib` 只剩 10 个 asan 变体，共 1.8 MB）。

这解释了移植后 launchd 为什么报：

```
Library not loaded: /usr/lib/libSystem.dylib
  Reason: tried: '/usr/lib/libSystem.dylib' (no such file, no dyld cache)
```

## 拿到之后：放在哪

- 这套 cache 约 3 GB，cust 只有 1 GiB，payload 塞不下。
- 本树**没有 FAT/msdosfs 驱动**，所以"挂 FAT 分区"这条路不通；APFS 又是只读挂载。
- 当前做法：打成带索引的 blob（`NABUDSC1`）写进 **userdata 分区容器之外的裸空间**
  （APFS 容器只占 6.08 GB，分区 498.75 GB），由内核侧一个极简只读文件系统挂到
  `/System/Library/Caches/com.apple.dyld`，dyld 本身一行都不用改。

> 写 userdata 裸空间不碰 APFS：容器 `nx_block_count = 1,483,776 × 4096 = 6.08 GB`，
> 写入点取 8 GiB，落点在容器之外；实测 10 GB / 400 GB 处都还是全 0。

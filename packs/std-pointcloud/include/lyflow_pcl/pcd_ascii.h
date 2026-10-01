#pragma once
// ASCII PCD 的快读快写。PCL 每读一个数走一次 istringstream / atof、每写一个数走一次 ostream，
// 200 万点的 ASCII 文件读 5 s、写 6 s；这里换成 from_chars / to_chars，结果与 PCL 1.12.0（vcpkg 装的那一版）
// 逐字节相同：文件头、二进制两种格式、以及正文里任何不是「普通十进制数」的词（nan、inf、+1、0x10、
// 溢出、次正规数……）都照旧交给 PCL 自己那一套。等价性由 tests/test_io_pcd.cpp 拿 PCL 本身对照。
#include <pcl/PCLPointCloud2.h>

#include <string>

namespace lyflow::ops::io {

/// 等同 `pcl::io::loadPCDFile(file, cloud)`：返回值、cloud 的每个字段与 data 的每个字节都一样。
/// `file` 是 PclPath 给的窄字符串；`threads` 是节点的线程预算，ASCII 正文按它分块并行（结果与线程数无关）。
int loadPcd(const std::string& file, pcl::PCLPointCloud2& cloud, int threads = 1);

/// 等同 `pcl::io::savePCDFile(file, cloud, 原点 0, 单位朝向, binary_mode=false)`：写出的文件逐字节相同。
int savePcdAscii(const std::string& file, const pcl::PCLPointCloud2& cloud, int threads = 1);

}  // namespace lyflow::ops::io

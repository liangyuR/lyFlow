// 算子包对象库的占位 TU：不带包构建时它是 lyflow_core_packs 里唯一的源文件。
// 空 TU 会让链接器报 LNK4221，所以留一个符号。
namespace lyflow::packs {
int packPlaceholder() { return 0; }
}  // namespace lyflow::packs

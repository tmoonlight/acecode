// 覆盖 src/skills/skill_loader.cpp 的元数据截断:description / whenToUse 超预算
// 时必须回退到 UTF-8 序列边界,不得切在多字节字符中间。
//
// 背景 bug:skill_loader 的 truncate() 曾用原始字节 substr + "..." 截断。
// 当 description 超过 1024 字节且截断点落在多字节字符中间时,会留下
// "残缺引导字节 + '.'" 的非法序列(如 0xE9 0x94 + "..." 的 0x2E),进入
// skills 索引后,请求体 body.dump() 以 nlohmann type_error.316
// ("invalid UTF-8 byte at index …: 0x2E") 打挂整个请求。本测试锁定该回归。

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "skills/skill_loader.hpp"
#include "utils/encoding.hpp"

namespace {

using acecode::load_skill_from_dir;
using acecode::is_valid_utf8;

// 构造一个 SKILL.md,其 description 超过 1024 字节,且 1021 字节的
// 截断预算落在某个 3 字节汉字(如"错"= 0xE9 0x94 0x99)中间——正是线上
// ~/.agents/skills/lark-apps/SKILL.md 触发崩溃的形态。
TEST(SkillLoaderTruncation, DescriptionCutInsideMultiByteCharStaysValidUtf8) {
    const std::string base =
        "---\n"
        "name: trunc-utf8-test\n"
        "description: ";
    // truncate(..., 1024) 为 "..." 预留 3 字节,所以原文预算是 1021。
    // 让三字节的"错"从偏移 1020 开始,原实现会只保留它的首字节。
    std::string description =
        std::string(1020, 'a') + u8"\u9519" + std::string(300, 'b');
    ASSERT_GT(description.size(), static_cast<size_t>(1024));

    const std::string content =
        base + description + "\n"
        "---\n"
        "body\n";

    const std::string dir = "trunc-utf8-test";
    // 写入临时目录
    const auto tmp_root = std::filesystem::temp_directory_path();
    const auto skill_dir = tmp_root / "acecode_skill_loader_utf8_test" / dir;
    std::filesystem::create_directories(skill_dir);
    const auto skill_md = skill_dir / "SKILL.md";
    {
        std::ofstream ofs(skill_md, std::ios::binary);
        ofs << content;
    }

    const auto scan_root = tmp_root / "acecode_skill_loader_utf8_test";
    const auto meta = load_skill_from_dir(skill_dir, scan_root);
    ASSERT_TRUE(meta.has_value());

    // 核心断言:截断后的 description 必须是合法 UTF-8(回归前会失败)。
    EXPECT_TRUE(is_valid_utf8(meta->description))
        << "description after truncation must remain valid UTF-8";
    // 且带有截断后缀(说明确实触发了截断路径)。
    // 预算 1021 字节 + "..."(3 字节) = 至多 1024;若 1021 落在多字节
    // 字符中间则回退到序列边界,总长只会更短。
    EXPECT_EQ(meta->description, std::string(1020, 'a') + "...");

    std::filesystem::remove_all(tmp_root / "acecode_skill_loader_utf8_test");
}

// whenToUse 同样走 truncate 路径,验证同一条防御。
TEST(SkillLoaderTruncation, WhenToUseCutInsideMultiByteCharStaysValidUtf8) {
    // 这次让"错"从偏移 1019 开始,截断预算会落在它的第三个字节上,
    // 从而同时覆盖需要回退两个 continuation byte 的情况。
    const std::string when =
        std::string(1019, 'x') + u8"\u9519" + std::string(300, 'y');
    const std::string content =
        "---\n"
        "name: trunc-when-test\n"
        "description: short\n"
        "whenToUse: " + when + "\n"
        "---\n"
        "body\n";

    const auto tmp_root = std::filesystem::temp_directory_path();
    const auto skill_dir = tmp_root / "acecode_skill_loader_utf8_test2" / "trunc-when-test";
    std::filesystem::create_directories(skill_dir);
    const auto skill_md = skill_dir / "SKILL.md";
    {
        std::ofstream ofs(skill_md, std::ios::binary);
        ofs << content;
    }

    const auto scan_root = tmp_root / "acecode_skill_loader_utf8_test2";
    const auto meta = load_skill_from_dir(skill_dir, scan_root);
    ASSERT_TRUE(meta.has_value());
    EXPECT_TRUE(is_valid_utf8(meta->when_to_use))
        << "when_to_use after truncation must remain valid UTF-8";
    EXPECT_EQ(meta->when_to_use, std::string(1019, 'x') + "...");

    std::filesystem::remove_all(tmp_root / "acecode_skill_loader_utf8_test2");
}

} // namespace

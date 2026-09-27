#include <gtest/gtest.h>

#include "llm/tool_protocol_names.hpp"
#include "tool/file_write_tool.hpp"
#include "tool/safe_text_write.hpp"
#include "tool/text_file_errors.hpp"
#include "utils/utf8_path.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path temp_path(const std::string& suffix) {
    static std::atomic<int> seq{0};
    return fs::temp_directory_path() /
        ("acecode_safe_text_write_" + std::to_string(++seq) + suffix);
}

void write_bytes(const fs::path& path, const std::string& bytes) {
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_bytes(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
}

} // namespace

// 场景:目标编码不可写时,安全写必须拒绝并保留原文件。沿用搬迁前的用例名。
TEST(TextFileBuffer, SafeWriteRollsBackOnVerificationMismatch) {
    auto path = temp_path(".txt");
    write_bytes(path, "before\n");

    auto metadata = acecode::default_new_file_text_metadata();
    metadata.encoding = acecode::TextEncoding::Unsupported;

    auto result = acecode::safe_write_text_file(path.string(), "after\n", metadata);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(read_bytes(path), "before\n");

    fs::remove(path);
}

// 回归:文案迁出底层后,严格读取和写工具仍须使用当前工具名映射,且不能覆盖歧义编码文件。
TEST(TextFileToolErrors, AmbiguousFilesKeepMappedGuidanceAndOriginalBytes) {
    acecode::ScopedModelToolNameMappings names({{"file_read", "inspect_file"}});
    const std::string damaged = std::string(u8"中文\n") + static_cast<char>(0xE4);
    struct Input {
        std::string bytes;
        std::string expected_error;
    };
    const Input inputs[] = {
        {std::string("\xEF\xBB\xBF", 3) + damaged,
         "[Error] UTF-8 BOM file can be read with inspect_file using lossy decoding, but its bytes are too ambiguous to edit safely."},
        {damaged,
         "[Error] File can be read with inspect_file using lossy UTF-8 decoding, but its encoding is too ambiguous to edit safely."},
        {std::string(1, static_cast<char>(0x81)),
         "[Error] File can be read with inspect_file using lossy decoding, but its encoding is too ambiguous to edit safely."},
    };
    auto tool = acecode::create_file_write_tool();
    for (const auto& input : inputs) {
        const auto path = temp_path(".txt");
        write_bytes(path, input.bytes);
        const auto utf8_path = acecode::path_to_utf8(path);
        const auto result = tool.execute(
            nlohmann::json{{"file_path", utf8_path}, {"content", "replacement"}}.dump(),
            acecode::ToolContext{});
        EXPECT_FALSE(result.success);
        EXPECT_EQ(result.output, input.expected_error);
        EXPECT_EQ(read_bytes(path), input.bytes);

        // 文件 API 也从严格读取结果取得文案;两处错误字段必须保持一致。
        const auto decoded = acecode::with_text_file_tool_errors(
            acecode::read_text_file_buffer(utf8_path));
        EXPECT_EQ(decoded.error, input.expected_error);
        EXPECT_EQ(decoded.buffer.metadata.error, input.expected_error);
        fs::remove(path);
    }
}

// 场景:有损读取的元数据不能用于回写;提示保留工具别名,写前回调也不得触发。
TEST(TextFileToolErrors, LossyWriteKeepsMappedGuidanceAndDoesNotRunWriteHook) {
    acecode::ScopedModelToolNameMappings names({{"file_read", "inspect_file"}});
    const auto path = temp_path(".txt");
    const std::string original = std::string(u8"中文\n") + static_cast<char>(0xE4);
    write_bytes(path, original);
    const auto utf8_path = acecode::path_to_utf8(path);
    auto decoded = acecode::read_text_file_buffer(utf8_path, true);
    ASSERT_TRUE(decoded.success) << decoded.error;
    ASSERT_TRUE(decoded.buffer.metadata.lossy);
    bool hook_called = false;
    const auto result = acecode::safe_write_text_file(
        utf8_path, "replacement", decoded.buffer.metadata,
        [&hook_called](const std::string&) { hook_called = true; });
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(hook_called);
    EXPECT_EQ(result.error,
        "[Error] Target file cannot be safely written as text because it was decoded lossily. Use inspect_file for inspection and convert the file to a confirmed encoding before editing.");
    EXPECT_EQ(read_bytes(path), original);
    fs::remove(path);
}

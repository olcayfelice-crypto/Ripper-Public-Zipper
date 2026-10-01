#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <zlib.h>

namespace fs = std::filesystem;

static constexpr char MAGIC[] = "RPZ1";
static constexpr uint32_t VERSION = 1;

struct FileEntry {
    std::string path;
    uint8_t method;
    uint64_t original_size;
    uint64_t compressed_size;
    uint64_t data_offset;
};

static void write_u8(std::ofstream& out, uint8_t v) {
    out.write(reinterpret_cast<const char*>(&v), 1);
}

static void write_u32(std::ofstream& out, uint32_t v) {
    out.write(reinterpret_cast<const char*>(&v), 4);
}

static void write_u64(std::ofstream& out, uint64_t v) {
    out.write(reinterpret_cast<const char*>(&v), 8);
}

static uint8_t read_u8(std::ifstream& in) {
    uint8_t v{};
    in.read(reinterpret_cast<char*>(&v), 1);
    return v;
}

static uint32_t read_u32(std::ifstream& in) {
    uint32_t v{};
    in.read(reinterpret_cast<char*>(&v), 4);
    return v;
}

static uint64_t read_u64(std::ifstream& in) {
    uint64_t v{};
    in.read(reinterpret_cast<char*>(&v), 8);
    return v;
}

static void write_string(std::ofstream& out, const std::string& s) {
    write_u32(out, static_cast<uint32_t>(s.size()));
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

static std::string read_string(std::ifstream& in) {
    uint32_t size = read_u32(in);

    std::string s(size, '\0');
    in.read(s.data(), static_cast<std::streamsize>(size));

    return s;
}

static bool should_store(const fs::path& p) {
    std::string ext = p.extension().string();

    std::transform(
        ext.begin(),
        ext.end(),
        ext.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    return ext == ".cpp" || ext == ".java";
}

static std::vector<unsigned char> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);

    if (!in)
        throw std::runtime_error("Dosya acilamadi: " + path.string());

    in.seekg(0, std::ios::end);
    std::streamsize size = in.tellg();
    in.seekg(0);

    std::vector<unsigned char> data(
        static_cast<size_t>(size)
    );

    if (size > 0)
        in.read(
            reinterpret_cast<char*>(data.data()),
            size
        );

    return data;
}

static std::vector<unsigned char> compress_data(
    const std::vector<unsigned char>& input
) {
    if (input.empty())
        return {};

    uLongf destination_size =
        compressBound(static_cast<uLong>(input.size()));

    std::vector<unsigned char> output(destination_size);

    int result = compress2(
        output.data(),
        &destination_size,
        input.data(),
        static_cast<uLong>(input.size()),
        Z_BEST_COMPRESSION
    );

    if (result != Z_OK)
        throw std::runtime_error("Sikistirma basarisiz.");

    output.resize(destination_size);

    return output;
}

static std::vector<unsigned char> decompress_data(
    const std::vector<unsigned char>& input,
    uint64_t original_size
) {
    std::vector<unsigned char> output(
        static_cast<size_t>(original_size)
    );

    if (original_size == 0)
        return output;

    uLongf size = static_cast<uLongf>(original_size);

    int result = uncompress(
        output.data(),
        &size,
        input.data(),
        static_cast<uLong>(input.size())
    );

    if (result != Z_OK || size != original_size)
        throw std::runtime_error("Arsiv verisi bozuk.");

    return output;
}

static void write_data(
    std::ofstream& out,
    const std::vector<unsigned char>& data
) {
    if (!data.empty()) {
        out.write(
            reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size())
        );
    }
}

static void pack(
    const fs::path& source,
    const fs::path& output
) {
    std::vector<FileEntry> entries;
    std::vector<std::vector<unsigned char>> blocks;

    for (const auto& item :
         fs::recursive_directory_iterator(source)) {

        if (!item.is_regular_file())
            continue;

        auto relative =
            fs::relative(item.path(), source);

        auto original = read_file(item.path());

        FileEntry entry;

        entry.path = relative.generic_string();
        entry.original_size = original.size();
        entry.data_offset = 0;

        if (should_store(item.path())) {

            entry.method = 0;
            entry.compressed_size = original.size();

            blocks.push_back(std::move(original));

        } else {

            auto compressed =
                compress_data(original);

            if (compressed.size() < original.size()) {

                entry.method = 1;
                entry.compressed_size =
                    compressed.size();

                blocks.push_back(
                    std::move(compressed)
                );

            } else {

                entry.method = 0;
                entry.compressed_size =
                    original.size();

                blocks.push_back(
                    std::move(original)
                );
            }
        }

        entries.push_back(std::move(entry));
    }

    std::ofstream out(
        output,
        std::ios::binary |
        std::ios::trunc
    );

    if (!out)
        throw std::runtime_error(
            "RPZ dosyasi olusturulamadi."
        );

    /*
       Header
    */

    out.write(MAGIC, 4);
    write_u32(out, VERSION);

    write_u32(
        out,
        static_cast<uint32_t>(entries.size())
    );

    /*
       Index
    */

    for (auto& entry : entries) {

        write_string(out, entry.path);

        write_u8(out, entry.method);

        write_u64(
            out,
            entry.original_size
        );

        write_u64(
            out,
            entry.compressed_size
        );
    }

    /*
       Data
    */

    for (size_t i = 0; i < entries.size(); ++i) {

        entries[i].data_offset =
            static_cast<uint64_t>(out.tellp());

        write_data(out, blocks[i]);
    }

    std::cout << "\nRPZ olusturuldu:\n";
    std::cout << output << "\n\n";

    for (const auto& e : entries) {

        std::cout
            << (e.method == 0 ? "[STORE] " : "[COMPRESS] ")
            << e.path
            << "  "
            << e.original_size
            << " -> "
            << e.compressed_size
            << " bytes\n";
    }
}

static std::vector<FileEntry> read_index(
    std::ifstream& in
) {
    char magic[4];

    in.read(magic, 4);

    if (std::string(magic, 4) != "RPZ1")
        throw std::runtime_error(
            "Gecerli bir RPZ dosyasi degil."
        );

    uint32_t version = read_u32(in);

    if (version != VERSION)
        throw std::runtime_error(
            "Desteklenmeyen RPZ surumu."
        );

    uint32_t count = read_u32(in);

    std::vector<FileEntry> entries;

    for (uint32_t i = 0; i < count; ++i) {

        FileEntry e;

        e.path = read_string(in);
        e.method = read_u8(in);
        e.original_size = read_u64(in);
        e.compressed_size = read_u64(in);

        e.data_offset = 0;

        entries.push_back(std::move(e));
    }

    uint64_t data_start =
        static_cast<uint64_t>(in.tellg());

    for (auto& e : entries) {

        e.data_offset = data_start;

        data_start += e.compressed_size;

        in.seekg(
            static_cast<std::streamoff>(e.compressed_size),
            std::ios::cur
        );
    }

    return entries;
}

static void list_archive(
    const fs::path& archive
) {
    std::ifstream in(
        archive,
        std::ios::binary
    );

    if (!in)
        throw std::runtime_error(
            "RPZ dosyasi acilamadi."
        );

    auto entries = read_index(in);

    std::cout << "RPZ archive\n\n";

    for (const auto& e : entries) {

        std::cout
            << (e.method == 0 ? "STORE    " : "COMPRESS ")
            << e.path
            << " | "
            << e.original_size
            << " -> "
            << e.compressed_size
            << "\n";
    }
}

static void extract_archive(
    const fs::path& archive,
    const fs::path& destination
) {
    std::ifstream in(
        archive,
        std::ios::binary
    );

    if (!in)
        throw std::runtime_error(
            "RPZ dosyasi acilamadi."
        );

    auto entries = read_index(in);

    for (const auto& e : entries) {

        in.seekg(
            static_cast<std::streamoff>(e.data_offset)
        );

        std::vector<unsigned char> data(
            static_cast<size_t>(e.compressed_size)
        );

        if (!data.empty()) {
            in.read(
                reinterpret_cast<char*>(data.data()),
                static_cast<std::streamsize>(data.size())
            );
        }

        std::vector<unsigned char> original;

        if (e.method == 0) {

            original = std::move(data);

        } else {

            original =
                decompress_data(
                    data,
                    e.original_size
                );
        }

        fs::path target =
            destination / fs::path(e.path);

        fs::create_directories(
            target.parent_path()
        );

        std::ofstream out(
            target,
            std::ios::binary |
            std::ios::trunc
        );

        if (!out)
            throw std::runtime_error(
                "Dosya yazilamadi: " +
                target.string()
            );

        if (!original.empty()) {
            out.write(
                reinterpret_cast<const char*>(
                    original.data()
                ),
                static_cast<std::streamsize>(
                    original.size()
                )
            );
        }

        std::cout
            << "Cikarildi: "
            << e.path
            << "\n";
    }
}

static void usage() {
    std::cout <<
R"(RPZ - Ripper - Public Zipper

Kullanim:

  rpz pack <klasor> <arsiv.rpz>
  rpz list <arsiv.rpz>
  rpz extract <arsiv.rpz> <hedef>

Ornek:

  rpz pack MyProject MyProject.rpz
  rpz list MyProject.rpz
  rpz extract MyProject.rpz output

.cpp ve .java dosyalari STORE olarak tutulur.
Diger dosyalar maksimum sikistirma denenerek saklanir.
)";
}

int main(int argc, char* argv[]) {

    try {

        if (argc < 2) {
            usage();
            return 1;
        }

        std::string command = argv[1];

        if (command == "pack") {

            if (argc != 4) {
                usage();
                return 1;
            }

            pack(
                argv[2],
                argv[3]
            );

            return 0;
        }

        if (command == "list") {

            if (argc != 3) {
                usage();
                return 1;
            }

            list_archive(argv[2]);

            return 0;
        }

        if (command == "extract") {

            if (argc != 4) {
                usage();
                return 1;
            }

            extract_archive(
                argv[2],
                argv[3]
            );

            return 0;
        }

        usage();
        return 1;

    } catch (const std::exception& e) {

        std::cerr
            << "RPZ ERROR: "
            << e.what()
            << '\n';

        return 1;
    }
}
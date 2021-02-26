#include <iostream>
#include <filesystem>
namespace fs = std::filesystem;

#include "CLI11.hpp"

#include "compressor.hpp"
#include "decompressor.hpp"

int main(int argc, const char *argv[]) {

    CLI::App app{"VCF/BCF Compressor"};

    std::string filename = "-";
    app.add_option("-f,--file", filename, "A help string");
    std::string ofname = "-";
    app.add_option("-o,--output", ofname, "Output file name, default is stdio");
    //char O = 'u';
    //app.add_option("-O, --output-type", O, "output type b|u|z|v");
    bool compress = false;
    bool decompress = false;
    bool info = false;
    bool wait = false;
    app.add_flag("-c,--compress", compress, "Compress");
    app.add_flag("-x,--extract", decompress, "Extract (Decompress)");
    app.add_flag("-i,--info", info, "Get info on file");
    app.add_flag("--wait", wait, "DEBUG - wait for int input");

    CLI11_PARSE(app, argc, argv);

    if (wait) {
        // Wait for input
        int _;
        std::cin >> _;
    }

    if (info) {
        if (filename.compare("-") == 0) {
            std::cerr << "INFO : Input is stdin" << std::endl;
        } else {
            std::cerr << "INFO : File is " << filename << std::endl;
            std::cerr << "INFO : " << filename << " size is " << fs::file_size(filename) << " bytes" << std::endl;
            std::string variants(filename + "_var.bcf");
            if (fs::exists(variants)) {
                std::cerr << "INFO : " << variants << " size is " << fs::file_size(variants) << " bytes" << std::endl;
            }
            header_t hdr;
            int ret = fill_header_from_file(filename, hdr);
            if (ret == 0) {
                std::cerr << "INFO : Header is\t\t\t" << sizeof(header_t) << " bytes" << std::endl;
                std::cerr << "INFO : Indices is\t\t\t" << hdr.ssas_offset - hdr.indices_offset << " bytes" << std::endl;
                std::cerr << "INFO : Subsampled permutation arrays is\t" << hdr.wahs_offset - hdr.ssas_offset << " bytes" << std::endl;
                std::cerr << "INFO : WAH Genotype data is\t\t" << hdr.samples_offset - hdr.wahs_offset << " bytes" << std::endl;
                std::cerr << "INFO : Samples list is\t\t\t" << fs::file_size(filename) - hdr.samples_offset << " bytes" << std::endl;
            }
        }
        std::cerr << std::endl;
    }

    if (compress && decompress) {
        std::cerr << "Cannot both compress and decompress, choose one" << std::endl << std::endl;
        exit(app.exit(CLI::CallForHelp()));
    } else if (compress) {
        /// @todo query overwrites

        if(filename.compare("-") and !fs::exists(filename)) {
            std::cerr << "File " << filename << " does not exist" << std::endl;
            exit(app.exit(CLI::RuntimeError()));
        }
        if(ofname.compare("-") == 0) {
            std::cerr << "Cannot output compressed file(s) to stdout" << std::endl << std::endl;
            exit(app.exit(CLI::CallForHelp()));
        }

        std::string variant_file(ofname + "_var.bcf");
        remove_samples(filename, variant_file);

        std::cout << "Generated file " << variant_file << " containing variants only" << std::endl;
        Compressor c;
        c.compress_in_memory(filename);
        std::cout << "Compressed filename " << filename << " in memory, now writing file " << ofname << std::endl;
        c.save_result_to_file(ofname);
        std::cout << "File " << ofname << " written" << std::endl;
    } else if (decompress) {
        /// @todo query overwrites

        if(filename.compare("-") == 0) {
            std::cerr << "Cannot decompress file(s) from stdin" << std::endl << std::endl;
            exit(app.exit(CLI::CallForHelp()));
        }
        if(!fs::exists(filename)) {
            std::cerr << "File " << filename << " does not exist" << std::endl;
            exit(app.exit(CLI::RuntimeError()));
        }

        std::string variant_file(filename + "_var.bcf");
        create_index_file(variant_file);
        Decompressor d(filename, variant_file);
        d.decompress(ofname);
    } else {
        std::cerr << "Choose either to compress or decompress" << std::endl << std::endl;
        exit(app.exit(CLI::CallForHelp()));
    }

    return 0;
}
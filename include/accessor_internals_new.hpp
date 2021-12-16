/*******************************************************************************
 * Copyright (C) 2021 Rick Wertenbroek, University of Lausanne (UNIL),
 * University of Applied Sciences and Arts Western Switzerland (HES-SO),
 * School of Management and Engineering Vaud (HEIG-VD).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 ******************************************************************************/
#ifndef __ACCESSOR_INTERNALS_NEW_HPP__
#define __ACCESSOR_INTERNALS_NEW_HPP__

#include <iostream>

#include <string>
#include <unordered_map>
#include "compression.hpp"
#include "xcf.hpp"
#include "gt_block.hpp"
#include "make_unique.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fs.hpp"
#include "wah.hpp"

using namespace wah;

#include "accessor_internals.hpp" // For DecompressPointer, AccessorInternals
#include "interfaces.hpp"

/// @todo DecompressPointer NEW version
template <typename A_T = uint32_t, typename WAH_T = uint16_t>
class DecompressPointerGTBlock : /* public DecompressPointer<A_T, WAH_T>, */ public GTBlockDict {
public:
    DecompressPointerGTBlock(const header_t& header, void* block_p) :
        header(header), block_p(block_p), N_HAPS(header.hap_samples),
        internal_binary_gt_line_position(0),
        y(header.hap_samples+sizeof(WAH_T)*8-1, false),
        a(header.hap_samples), b(header.hap_samples) {
        // Load dictionary
        read_dictionary(dictionary, (uint32_t*)block_p);
        bcf_lines_in_block = dictionary.at(KEY_BCF_LINES);
        binary_gt_lines_in_block = dictionary.at(KEY_BINARY_LINES);

        //std::cerr << "Created a new GTB decompress pointer with " << bcf_lines_in_block << " bcf lines and " << binary_gt_lines_in_block << " binary lines" << std::endl;

        // Load dim-1 structures
        fill_bool_vector_from_1d_dict_key(KEY_LINE_SELECT, binary_gt_line_is_wah, binary_gt_lines_in_block);
        if (!fill_bool_vector_from_1d_dict_key(KEY_LINE_SORT, binary_gt_line_is_sorting, binary_gt_lines_in_block)) {
            // By default only the wah lines are sorting
            binary_gt_line_is_sorting = binary_gt_line_is_wah;
        }
        fill_bool_vector_from_1d_dict_key(KEY_LINE_MISSING, line_has_missing, bcf_lines_in_block);
        fill_bool_vector_from_1d_dict_key(KEY_LINE_NON_UNIFORM_PHASING, line_has_non_uniform_phasing, bcf_lines_in_block);
        fill_bool_vector_from_1d_dict_key(KEY_LINE_END_OF_VECTORS, line_has_end_of_vector, bcf_lines_in_block);
        fill_bool_vector_from_1d_dict_key(KEY_LINE_HAPLOID, haploid_binary_gt_line, binary_gt_lines_in_block);
        if (!haploid_binary_gt_line.size()) { haploid_binary_gt_line.resize(binary_gt_lines_in_block, false); }

        /// @todo non default vector lengths

        // Set access to 2D structures (don't decompress them unless needed)
        wah_origin_p = get_pointer_from_dict<WAH_T>(KEY_MATRIX_WAH);
        wah_p = wah_origin_p;

        sparse_origin_p = get_pointer_from_dict<A_T>(KEY_MATRIX_SPARSE);
        sparse_p = sparse_origin_p;

        std::iota(a.begin(), a.end(), 0);
    }
    virtual ~DecompressPointerGTBlock() {}

    /**
     * @brief Updates all internal structures to point to the requested binary gt entry
     * */
    inline void seek(const size_t position) /*override*/ {
        if (internal_binary_gt_line_position == position) {
            return;
        } else {
            if (internal_binary_gt_line_position > position) {
                std::cerr << "Slow backwards seek !" << std::endl;
                reset();
            }
            while (internal_binary_gt_line_position < position) {
                if (binary_gt_line_is_wah[internal_binary_gt_line_position]) {
                    /// @todo
                    // Resize y based on the vector length
                    const size_t CURRENT_N_HAPS = (haploid_binary_gt_line[internal_binary_gt_line_position] ? N_HAPS >> 1 : N_HAPS);
                    if (binary_gt_line_is_sorting[internal_binary_gt_line_position]) {
                        wah_p = wah2_extract(wah_p, y, CURRENT_N_HAPS);
                    } else {
                        /* reference advance */ wah2_advance_pointer(wah_p, CURRENT_N_HAPS);
                    }
                } else {
                    // Is sparse
                    if (binary_gt_line_is_sorting[internal_binary_gt_line_position]) {
                        sparse_p = sparse_extract(sparse_p);
                    } else {
                        sparse_p = sparse_advance_pointer(sparse_p);
                    }
                }
                update_a_if_needed();

                internal_binary_gt_line_position++;
            }
        }
    }

    inline void fill_genotype_array_advance(int32_t* gt_arr, size_t gt_arr_size, size_t n_alleles) {
        allele_counts.resize(n_alleles);
        size_t total_alt = 0;
        int32_t DEFAULT_PHASED = dictionary[KEY_DEFAULT_PHASING]; /// @todo

        const size_t CURRENT_N_HAPS = (haploid_binary_gt_line[internal_binary_gt_line_position] ? N_HAPS >> 1 : N_HAPS);

        // Set REF / first ALT
        if (!binary_gt_line_is_wah[internal_binary_gt_line_position]) { /* SPARSE */
            sparse_p = sparse_extract(sparse_p);
            int32_t default_gt = sparse_negated ? 1 : 0;
            int32_t sparse_gt = sparse_negated ? 0 : 1;

            for (size_t i = 0; i < CURRENT_N_HAPS; ++i) {
                gt_arr[i] = bcf_gt_unphased(default_gt) | ((i & 1) & DEFAULT_PHASED);
            }
            for (const auto& i : sparse) {
                //if constexpr (DEBUG_DECOMP) std::cerr << "Setting variant at " << i << std::endl;
                gt_arr[i] = bcf_gt_unphased(sparse_gt) | ((i & 1) & DEFAULT_PHASED);
            }
        } else { /* SORTED WAH */
            wah_p = wah2_extract_count_ones(wah_p, y, CURRENT_N_HAPS, ones);

            for (size_t i = 0; i < CURRENT_N_HAPS; ++i) {
                gt_arr[a[i]] = bcf_gt_unphased(y[i]) | ((a[i] & 1) & DEFAULT_PHASED); /// @todo Phase
            }
        }

        allele_counts[1] = ones;
        total_alt = ones;
        update_a_if_needed();
        internal_binary_gt_line_position++;

        // If other ALTs (ALTs are 1 indexed, because 0 is REF)
        for (size_t alt_allele = 2; alt_allele < n_alleles; ++alt_allele) {
            if (!binary_gt_line_is_wah[internal_binary_gt_line_position]) { /* SPARSE */
                sparse_p = sparse_extract(sparse_p);
                if (sparse_negated) { // There can only be one negated because must be more than all others combined
                    // All non set positions are now filled
                    for (size_t i = 0; i < CURRENT_N_HAPS; ++i) {
                        // Only overwrite refs
                        if (bcf_gt_allele(gt_arr[i]) == 0) {
                            gt_arr[i] = bcf_gt_unphased(alt_allele) | ((i & 1) & DEFAULT_PHASED);
                        }
                    }
                    for (const auto& i : sparse) {
                        // Restore overwritten refs
                        if (bcf_gt_allele(gt_arr[i]) == (int)alt_allele) {
                            gt_arr[i] = bcf_gt_unphased(0) | ((i & 1) & DEFAULT_PHASED);
                        }
                    }
                } else {
                    // Fill normally
                    for (const auto& i : sparse) {
                        gt_arr[i] = bcf_gt_unphased(alt_allele) | ((i & 1) & DEFAULT_PHASED);
                    }
                }
            } else { /* SORTED WAH */
                wah_p = wah2_extract_count_ones(wah_p, y, CURRENT_N_HAPS, ones);
                for (size_t i = 0; i < CURRENT_N_HAPS; ++i) {
                    if (y[i]) {
                        gt_arr[a[i]] = bcf_gt_unphased(alt_allele) | ((a[i] & 1) & DEFAULT_PHASED); /// @todo Phase
                    }
                }
            }
            allele_counts[alt_allele] = ones;
            total_alt += ones;
            update_a_if_needed();
            internal_binary_gt_line_position++;
        }

        allele_counts[0] = CURRENT_N_HAPS - total_alt;
        /// @todo Apply Missing, phase, eov...
    }

    void reset() {
        wah_p = wah_origin_p;
        sparse_p = sparse_origin_p;
        std::iota(a.begin(), a.end(), 0);

        internal_binary_gt_line_position = 0;
    }

    inline void fill_allele_counts_advance(const size_t n_alleles) {
        allele_counts.resize(n_alleles);
        size_t total_alt = 0;

        const size_t CURRENT_N_HAPS = (haploid_binary_gt_line[internal_binary_gt_line_position] ? N_HAPS >> 1 : N_HAPS);

        for (size_t alt_allele = 1; alt_allele < n_alleles; ++alt_allele) {
            if (binary_gt_line_is_wah[internal_binary_gt_line_position]) {
                /// @todo
                // Resize y based on the vector length
                if (binary_gt_line_is_sorting[internal_binary_gt_line_position]) {
                    wah_p = wah2_extract_count_ones(wah_p, y, CURRENT_N_HAPS, ones);
                } else {
                    /* reference advance */ ones = wah2_advance_pointer_count_ones(wah_p, CURRENT_N_HAPS);
                }
            } else {
                // Is sparse (both methods count ones)
                if (binary_gt_line_is_sorting[internal_binary_gt_line_position]) {
                    sparse_p = sparse_extract(sparse_p);
                } else {
                    sparse_p = sparse_advance_pointer(sparse_p);
                }
            }
            update_a_if_needed();
            internal_binary_gt_line_position++;

            allele_counts[alt_allele] = ones;
            total_alt += ones;
        }

        allele_counts[0] = CURRENT_N_HAPS - total_alt; // - total missing ?
    }

    inline const std::vector<size_t>& get_allele_count_ref() const {
        return allele_counts;
    }

protected:
    template<const size_t V_LEN_RATIO = 1>
    inline void private_pbwt_sort() {
        size_t u = 0;
        size_t v = 0;
        // PBWT sort
        for (size_t i = 0; i < N_HAPS; ++i) {
            if (this->y[i/V_LEN_RATIO] == 0) {
                this->a[u++] = this->a[i];
            } else {
                this->b[v++] = this->a[i];
            }
        }
        std::copy(this->b.begin(), this->b.begin()+v, this->a.begin()+u);
    }

    inline void update_a_if_needed() {
        // Extracted line is used to sort
        if (binary_gt_line_is_sorting[internal_binary_gt_line_position]) {
            if (haploid_binary_gt_line[internal_binary_gt_line_position]) {
                private_pbwt_sort<2>();
            } else {
                private_pbwt_sort<1>();
            }
        }
    }

    inline bool fill_bool_vector_from_1d_dict_key(enum Dictionary_Keys key, std::vector<bool>& v, const size_t size) {
        if (dictionary.find(key) != dictionary.end()) {
            v.resize(size+sizeof(WAH_T)*8-1);
            WAH_T* wah_p = (WAH_T*)(((char*)block_p)+dictionary[key]);
            wah2_extract<WAH_T>(wah_p, v, size);
            return true;
        } else {
            return false;
        }
    }

    template<typename T>
    inline T* get_pointer_from_dict(enum Dictionary_Keys key) {
        if (dictionary.find(key) != dictionary.end()) {
            return (T*)(((char*)block_p)+dictionary[key]);
        } else {
            return nullptr;
        }
    }

    A_T* sparse_extract(A_T* s_p) {
        constexpr A_T MSB_BIT = (A_T)1 << (sizeof(A_T)*8-1);
        A_T num = *s_p;
        s_p++;

        sparse_negated = (num & MSB_BIT);
        num &= ~MSB_BIT; // Remove the bit !

        sparse.clear();
        for (A_T i = 0; i < num; i++) {
            sparse.push_back(*s_p);
            s_p++;
        }

        const size_t CURRENT_N_HAPS = (haploid_binary_gt_line[internal_binary_gt_line_position] ? N_HAPS >> 1 : N_HAPS);
        ones = (sparse_negated ? CURRENT_N_HAPS-num : num);

        return s_p;
    }

    A_T* sparse_advance_pointer(A_T* s_p) {
        constexpr A_T MSB_BIT = (A_T)1 << (sizeof(A_T)*8-1);
        A_T num = *s_p;
        s_p++;

        sparse_negated = (num & MSB_BIT);
        num &= ~MSB_BIT; // Remove the bit !

        s_p += num;

        const size_t CURRENT_N_HAPS = (haploid_binary_gt_line[internal_binary_gt_line_position] ? N_HAPS >> 1 : N_HAPS);
        ones = (sparse_negated ? CURRENT_N_HAPS-num : num);

        return s_p;
    }

protected:
    const header_t& header;
    const void* block_p;
    const size_t N_HAPS;
    size_t bcf_lines_in_block;
    size_t binary_gt_lines_in_block;

    std::map<uint32_t, uint32_t> dictionary;

    size_t internal_binary_gt_line_position;

    WAH_T* wah_origin_p;
    WAH_T* wah_p;
    A_T* sparse_origin_p;
    A_T* sparse_p;

    std::vector<size_t> sparse;
    bool sparse_negated;
    // WAH_T* missing_origin_p;
    // WAH_T* missing_p;
    // WAH_T* non_uniform_phasing_origin_p;
    // WAH_T* non_uniform_phasing_p;
    // WAH_T* eovs_origin_p;
    // WAH_T* eovs_p;

    std::vector<bool> binary_gt_line_is_wah;
    std::vector<bool> binary_gt_line_is_sorting;
    std::vector<bool> line_has_missing;
    std::vector<bool> line_has_non_uniform_phasing;
    std::vector<bool> line_has_end_of_vector;
    //std::map<size_t, int32_t> non_default_vector_length_positions;
    std::vector<bool> haploid_binary_gt_line;


    std::vector<size_t> allele_counts;
    size_t ones;

    // Internal
    std::vector<bool> y;
    std::vector<A_T> a, b;
};

template <typename A_T = uint32_t, typename WAH_T = uint16_t>
class AccessorInternalsNewTemplate : public AccessorInternals {
public:
    void fill_genotype_array(int32_t* gt_arr, size_t gt_arr_size, size_t n_alleles, size_t new_position) override {
        const size_t OFFSET_MASK = ~((((size_t)-1) >> BM_BLOCK_BITS) << BM_BLOCK_BITS);
        size_t block_id = ((new_position & 0xFFFFFFFF) >> BM_BLOCK_BITS);
        // The offset is relative to the start of the block and is binary gt lines
        uint32_t offset = new_position & OFFSET_MASK;

        // If block ID is not current block
        if (!dp or current_block != block_id) {
            set_gt_block_ptr(block_id);

            // Make DecompressPointer
            dp = make_unique<DecompressPointerGTBlock<A_T, WAH_T> >(header, gt_block_p);
            //std::cerr << "Block ID : " << block_id << " offset : " << offset << std::endl;
        }

        dp->seek(offset);
        dp->fill_genotype_array_advance(gt_arr, gt_arr_size, n_alleles);
    }

    void fill_allele_counts(size_t n_alleles, size_t new_position) override {
        // Conversion from new to old, for the moment
        size_t block_id = new_position >> BM_BLOCK_BITS;
        // The offset is relative to the start of the block and is binary gt lines
        int32_t offset = (new_position << (32-BM_BLOCK_BITS)) >> (32-BM_BLOCK_BITS);

        // If block ID is not current block
        if (!dp or current_block != block_id) {
            // Gets block from file (can decompress)
            set_gt_block_ptr(block_id);

            // Make DecompressPointer
            dp = make_unique<DecompressPointerGTBlock<A_T, WAH_T> >(header, gt_block_p);
        }

        dp->seek(offset);
        dp->fill_allele_counts_advance(n_alleles);
    }

    // Directly pass the DecompressPointer Allele counts
    virtual inline const std::vector<size_t>& get_allele_counts() const override {
        return dp->get_allele_count_ref();
    }

    AccessorInternalsNewTemplate(std::string filename) {
        std::fstream s(filename, s.binary | s.in);
        if (!s.is_open()) {
            std::cerr << "Failed to open file " << filename << std::endl;
            throw "Failed to open file";
        }

        // Read the header
        s.read((char *)(&(this->header)), sizeof(header_t));
        s.close();

        // Check magic
        if ((header.first_magic != MAGIC) or (header.last_magic != MAGIC)) {
            std::cerr << "Bad magic" << std::endl;
            std::cerr << "Expected : " << MAGIC << " got : " << header.first_magic << ", " << header.last_magic << std::endl;
            throw "Bad magic";
        }

        // Check version
        if (header.version != 4) {
            std::cerr << "Bad version" << std::endl;
            throw "Bad version";
        }

        file_size = fs::file_size(filename);
        fd = open(filename.c_str(), O_RDONLY, 0);
        if (fd < 0) {
            std::cerr << "Failed to open file " << filename << std::endl;
            throw "Failed to open file";
        }

        // Memory map the file
        file_mmap_p = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
        if (file_mmap_p == NULL) {
            std::cerr << "Failed to memory map file " << filename << std::endl;
            close(fd);
            throw "Failed to mmap file";
        }

        // Test the memory map (first thing is the endianness in the header)
        uint32_t endianness = *(uint32_t*)(file_mmap_p);
        if (endianness != ENDIANNESS) {
            std::cerr << "Bad endianness in memory map" << std::endl;
            throw "Bad endianness";
        }

        if (header.hap_samples == 0) {
            std::cerr << "No samples" << std::endl;
            // Can still be used to "extract" the variant BCF (i.e. loop through the variant BCF and copy it to output... which is useless but ok)
        }

        if (header.ploidy == 0) {
            std::cerr << "Ploidy in header is set to 0 !" << std::endl;
            throw "PLOIDY ERROR";
        }
    }

    virtual ~AccessorInternalsNewTemplate() {
        if (header.zstd and block_p) {
            free(block_p);
            block_p = nullptr;
        }
        munmap(file_mmap_p, file_size);
        close(fd);
    }

protected:
    inline void set_gt_block_ptr(const size_t block_id) {
        set_block_ptr(block_id);
        current_block = block_id;
        char* p = (char*)block_p;

        try {
            p += block_dictionary.at(IBinaryBlock<uint32_t, uint32_t>::KEY_GT_ENTRY);
        } catch (...) {
            std::cerr << "Binary block does not have GT block" << std::endl;
            throw "block error";
        }

        gt_block_p = p;
    }

    inline void set_block_ptr(const size_t block_id) {
        uint32_t* indices_p = (uint32_t*)((uint8_t*)file_mmap_p + header.indices_offset);
        // Find out the block offset
        size_t offset = indices_p[block_id];

        if (header.zstd) {
            size_t compressed_block_size = *(uint32_t*)(((uint8_t*)file_mmap_p) + offset);
            size_t uncompressed_block_size = *(uint32_t*)(((uint8_t*)file_mmap_p) + offset + sizeof(uint32_t));
            void *block_ptr = ((uint8_t*)file_mmap_p) + offset + sizeof(uint32_t)*2;

            if (block_p) {
                free(block_p);
                block_p = nullptr;
            }

            block_p = malloc(uncompressed_block_size);
            if (!block_p) {
                std::cerr << "Failed to allocate memory to decompress block" << std::endl;
                throw "Failed to allocate memory";
            }
            auto result = ZSTD_decompress(block_p, uncompressed_block_size, block_ptr, compressed_block_size);
            if (ZSTD_isError(result)) {
                std::cerr << "Failed to decompress block" << std::endl;
                std::cerr << "Error : " << ZSTD_getErrorName(result) << std::endl;
                throw "Failed to decompress block";
            }
        } else {
            // Set block pointer
            block_p = ((uint8_t*)file_mmap_p) + offset;
        }

        read_dictionary(block_dictionary, (uint32_t*)block_p);
    }

    std::string filename;
    header_t header;
    size_t file_size;
    int fd;
    void* file_mmap_p = nullptr;

    void* block_p = nullptr;
    void* gt_block_p = nullptr;
    std::unique_ptr<DecompressPointerGTBlock<A_T, WAH_T> > dp = nullptr;
    size_t current_block = -1;
    std::map<uint32_t, uint32_t> block_dictionary;
};

#endif /* __ACCESSOR_INTERNALS_NEW_HPP__ */
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <doca_error.h>
#include <doca_log.h>

#include "lz4_decomp.h"

DOCA_LOG_REGISTER(EC_CREATE : MAIN);

constexpr char data_paths[][128] = {
    "./lz4_test_files/compressed_16.lz4",
    "./lz4_test_files/compressed_32.lz4",
    "./lz4_test_files/compressed_64.lz4",
    "./lz4_test_files/compressed_128.lz4",
    "./lz4_test_files/compressed_256.lz4",
    "./lz4_test_files/compressed_512.lz4",
    "./lz4_test_files/compressed_1024.lz4",
    "./lz4_test_files/compressed_2048.lz4",
    "./lz4_test_files/compressed_4096.lz4",
    "./lz4_test_files/compressed_8192.lz4",
    "./lz4_test_files/compressed_16384.lz4",
    "./lz4_test_files/compressed_32768.lz4",
    "./lz4_test_files/compressed_65536.lz4",
    "./lz4_test_files/compressed_131072.lz4",
    "./lz4_test_files/compressed_262144.lz4",
    "./lz4_test_files/compressed_524288.lz4",
    "./lz4_test_files/compressed_1048576.lz4",
    // "./lz4_test_files/compressed_2097152.lz4",
};

bool
read_lz4_file(const std::string& filename,
              std::vector<uint8_t>& output_buffer,
              size_t& file_size)
{
    // Open file in binary mode
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }

    // Get file size
    file_size = file.tellg();
    if (file_size == 0) {
        file.close();
        return false;
    }

    // Resize output buffer
    output_buffer.resize(file_size);

    // Read file content
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(output_buffer.data()), file_size);

    // Check for read errors
    bool success = file.good();
    file.close();

    return success;
}

static doca_error_t
profile()
{
    doca_error_t status;

    for (uint32_t i = 0; i < 17; i++) {
        std::string path{ data_paths[i] };
        std::vector<uint8_t> data_buf;
        size_t data_size;
        read_lz4_file(path, data_buf, data_size);
        lz4_decomp_config cfg = { .input_size = data_size,
                                  .data = data_buf.data(),
                                  .nb_tasks = 32 };
        status = lz4_decomp(cfg);
        if (status != DOCA_SUCCESS) {
            DOCA_LOG_ERR("LZ4 decomp failed when ");
            return status;
        }
    }

    return DOCA_SUCCESS;
}

int
main(int argc, char** argv)
{
    doca_error_t status;

    /* Setup SDK logger */
    doca_log_backend* sdk_log;
    status = doca_log_backend_create_standard();
    if (status != DOCA_SUCCESS) {
        printf("Failed to create log standard backend: %s\n",
               doca_error_get_descr(status));
        return EXIT_FAILURE;
    }

    status = doca_log_backend_create_with_file_sdk(stderr, &sdk_log);
    if (status != DOCA_SUCCESS) {
        printf("Failed to create log backend with file sdk: %s\n",
               doca_error_get_descr(status));
        return EXIT_FAILURE;
    }

    status = doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING);
    if (status != DOCA_SUCCESS) {
        printf("Failed to set log backend level: %s",
               doca_error_get_descr(status));
        return EXIT_FAILURE;
    }

    status = profile();
    if (status != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Profiling failed");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
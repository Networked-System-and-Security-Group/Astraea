import lz4.block
import os
import random
import string

def generate_random_data(size):
    """Generate random data of specified size in bytes."""
    return ''.join(random.choices(string.ascii_letters + string.digits, k=size)).encode('utf-8')

def generate_lz4_files(output_dir, sizes):
    """
    Generate LZ4 block compressed files of different sizes.
    sizes: list of target uncompressed data sizes in bytes
    """
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    for size in sizes:
        # Generate random data
        raw_data = generate_random_data(size)
        
        # Compress using LZ4 block mode
        compressed_data = lz4.block.compress(raw_data, store_size=False)
        
        # Write to file
        output_file = os.path.join(output_dir, f'compressed_{size}.lz4')
        with open(output_file, 'wb') as f:
            f.write(compressed_data)
        
        print(f'Generated {output_file}: compressed size = {len(compressed_data)} bytes')

if __name__ == "__main__":
    # Define different sizes for testing (in bytes)
    test_sizes = [
        1024,          # 1 KB
        10240,         # 10 KB
        102400,        # 100 KB
        1048576,       # 1 MB
        10485760,      # 10 MB
        104857600      # 100 MB
    ]
    
    output_directory = "./lz4_test_files"
    generate_lz4_files(output_directory, test_sizes)
import ctypes
import os
import sys

# 1. Load the library
lib_path = os.path.abspath("./libqua.so")
try:
    lib = ctypes.CDLL(lib_path)
except OSError:
    print(f"Error: Cannot find {lib_path}. Run 'make' first.")
    sys.exit(1)

# 2. Configure Function Signatures
# We use c_void_p for the return to prevent Python from auto-converting it to a string,
# which allows us to manually free the C-allocated memory later.
lib.find_biggest_art.argtypes = [ctypes.c_char_p]
lib.find_biggest_art.restype = ctypes.c_void_p

# We need the standard C free() to clean up the strdup'd memory
libc = ctypes.CDLL("libc.so.6")
libc.free.argtypes = [ctypes.c_void_p]

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 test_qua.py <path_to_music_or_folder>")
        sys.exit(1)

    input_path = sys.argv[1]
    
    # Call the C library
    ptr = lib.find_biggest_art(input_path.encode('utf-8'))

    if ptr:
        # Convert the pointer to a Python string
        result = ctypes.string_at(ptr).decode('utf-8')
        print(result)
        
        # Free the memory allocated by strdup in C
        libc.free(ptr)
    else:
        # Print nothing or an error to stderr if not found
        print("No album art found.", file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()

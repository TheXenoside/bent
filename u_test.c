/*******************************************************************************************
 *
 * convey - Print Macros & Formatting Examples
 *
 * This example demonstrates every way to print and format strings, slices,
 * and UTF-16 data using convey's built-in macros.
 *
 ********************************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include "convey_main4.c"

int main(void) {
 // ===================================================================================
 // 1. SETUP: Create some strings to test with
 // ===================================================================================
 printf("--- INITIALIZING STRINGS ---\n");

 c_string_t my_str = c_string_create("Hello, Convey!");
 c_string_t messy_str = c_string_create("   Trim me!   ");

 // Create a UTF-16 string for the wide-character examples later
 c_string_t utf16_str = c_string_convert_utf8_utf16(my_str);

 printf("\n");

 // ===================================================================================
 // 2. THE STANDARD MACROS (C_STR_FMT & C_str_arg)
 // ===================================================================================
 // C_STR_FMT expands to "%.*s"
 // C_str_arg(X) expands to: C_str_len(X), C_str_ptr(X)
 // WARNING: C_str_arg evaluates its argument twice. Only use it with variables!

 printf("--- STANDARD MACROS ---\n");

 // Example A: Printing a dynamic c_string_t
 printf("A. Dynamic string: " C_STR_FMT "\n", C_str_arg(my_str));

 // Example B: Printing a standard C literal/pointer
 const char* standard_c_str = "Standard C Literal";
 printf("B. C-String literal: " C_STR_FMT "\n", C_str_arg(standard_c_str));

 // Example C: Printing a raw slice (e.g., just the "Hello" part)
 c_slice_t slice = C_SUBSLICE(my_str, 0, 5);
 printf("C. Slice (First 5 bytes): " C_STR_FMT "\n", C_str_arg(slice));

 // Example D: Printing a UTF-8 slice
 c_utf8_slice_t utf8_slice = C_UTF8_SLICE_OF(my_str);
 printf("D. UTF-8 Slice: " C_STR_FMT "\n", C_str_arg(utf8_slice));

 printf("\n");

 // ===================================================================================
 // 3. MANUAL BREAKDOWN (C_str_len & C_str_ptr)
 // ===================================================================================
 // If you need the length or pointer separately, you can call the underlying
 // macros that power C_str_arg().

 printf("--- MANUAL MACROS ---\n");

 int len = C_str_len(my_str);
 const char* ptr = C_str_ptr(my_str);

 printf("Manual extraction -> Length: %d, Pointer: %p, Data: %.*s\n", len, (void*)ptr, len, ptr);
 printf("\n");

 // ===================================================================================
 // 4. THE SAFE MACRO (C_PRINTF_SAFE)
 // ===================================================================================
 // Because C_str_arg(X) evaluates 'X' twice, you CANNOT put a function inside it.
 // Doing `C_str_arg(c_string_trim(str))` would call trim() twice!
 // Instead, use C_PRINTF_SAFE when calling functions.

 printf("--- SAFE PRINT MACRO ---\n");

 // Look at messy_str before trimming
 printf("Before safe print: [%s]\n", messy_str);  // using %s just to show the spaces

 // Safely print the result of an in-place mutation function
 // C_PRINTF_SAFE takes the format string, and the expression.
 C_PRINTF_SAFE("After safe print (Trimmed): [" C_STR_FMT "]\n", c_string_trim(messy_str));

 printf("\n");

 // ===================================================================================
 // 5. CROSS-PLATFORM UTF-16 PRINTING
 // ===================================================================================
 // UTF-16 doesn't play nicely with standard printf on many OSs.
 // convey provides a safe function to print UTF-16 to stdout on any platform.

 printf("--- CROSS-PLATFORM UTF-16 ---\n");

 printf("Printing UTF-16 safely: ");
 c_string_print_utf16(C_UTF16_SLICE_OF(utf16_str));
 printf("\n\n");

 // ===================================================================================
 // 6. WINDOWS / WIDE CHARACTER MACROS (wprintf)
 // ===================================================================================
 // If you are on Windows, or a platform where wchar_t is 16-bits, you can use
 // the wprintf variants. These are guarded by an #ifdef to prevent compilation
 // errors on Linux/Mac where wchar_t is 32-bits.

 printf("--- WINDOWS WIDE MACROS ---\n");

#if defined(_WIN32) || WCHAR_MAX == 0xFFFFu
 // C_WSTR_FMT expands to L"%.*ls"
 wprintf(L"Wide string: " C_WSTR_FMT L"\n", C_wstr_arg(utf16_str));

 // C_WPRINTF_SAFE is the single-evaluation equivalent for wide strings
 // Note: c_string_trim doesn't work on UTF16, so we'll just pass a slice constructor
 C_WPRINTF_SAFE(L"Wide safe print: [" C_WSTR_FMT L"]\n", C_UTF16_SLICE_OF(utf16_str));
#else
 printf("(Skipped: These macros only activate when _WIN32 is defined or WCHAR_MAX == 0xFFFF)\n");
#endif

 // ===================================================================================
 // 7. CLEANUP
 // ===================================================================================
 c_string_free(my_str);
 c_string_free(messy_str);
 c_string_free(utf16_str);

 return 0;
}

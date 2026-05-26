#include <stdio.h>
#include <stdlib.h>

namespace std
{
    void __throw_out_of_range_fmt(const char *, ...)
    {
        printf("C++ out_of_range exception requested.\n");
        abort();
    }
}

namespace __gnu_cxx
{
    void __verbose_terminate_handler()
    {
        printf("Unhandled C++ exception reached terminate.\n");
        abort();
    }
}

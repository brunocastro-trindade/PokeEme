#ifdef PORTABLE
#include "platform/gl_api.h"

#define GL_DEFINE_POINTER(ret, name, params) ret (GL_APIENTRY *pe_##name) params;
GL_FUNCTION_LIST(GL_DEFINE_POINTER)
#undef GL_DEFINE_POINTER

bool GlApi_Load(void *(*getProc)(const char *name), const char **missing)
{
#define GL_LOAD_POINTER(ret, name, params)          \
    pe_##name = (ret (GL_APIENTRY *) params)getProc(#name); \
    if (pe_##name == NULL)                           \
    {                                                \
        if (missing != NULL)                         \
            *missing = #name;                        \
        return false;                                \
    }
    GL_FUNCTION_LIST(GL_LOAD_POINTER)
#undef GL_LOAD_POINTER
    return true;
}

#endif // PORTABLE

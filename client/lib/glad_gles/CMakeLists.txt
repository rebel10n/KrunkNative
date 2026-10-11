cmake_minimum_required(VERSION 3.10)

# OpenGL ES 3.0 loader (glad 0.1.36, --api="gles2=3.0" --profile=core) for the ANGLE backend
# (KRUNKNATIVE_GL_BACKEND=angle). Same <glad/glad.h> include as client/lib/glad, so a target
# links exactly one of `glad` (desktop GL 4.5) or `glad_gles`.
add_library(glad_gles src/glad.c)
target_include_directories(glad_gles PUBLIC include)

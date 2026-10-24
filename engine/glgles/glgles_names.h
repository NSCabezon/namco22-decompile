/*
 * glgles/glgles_names.h -- namespacing for the GLES 3.0 backend's fixed-function GL subset (engine/glgles).
 *
 * On Android/Quest the final shared object (libspatialarcade.so) also contains the Quest SHELL (quest/src/render.c),
 * which calls the SAME standard gl* names against the system libGLESv3. If glgles DEFINED those names, the static linker
 * would bind the shell's gl* references to glgles's definitions inside the one .so -- hijacking the shell's own rendering
 * (black screen). -fvisibility=hidden does NOT prevent that intra-.so binding.
 *
 * So, exactly like third_party/musashi/n22_musashi_names.h renames Musashi's globals to n22_*, this header renames the GL
 * subset glgles implements to glgles_* . It is force-included (-include) ONLY into the engine/core render translation
 * units (cores/namco22 Android target: N22_CORE_SRC + glgles.c), NEVER into the Quest shell's own sources. The engine was
 * written for fixed-function GL and calls these gl* names; the #defines route those calls to glgles, while glgles's
 * definitions become glgles_* and the public gl* names are left UNDEFINED in the .so, so the shell's calls resolve to the
 * real libGLESv3 at load time.
 *
 * glgles itself still calls the REAL driver for the modern bits (FBOs, programs, sync, buffers) through its dlsym'd R_*
 * pointers, which load libGLESv3 explicitly by string and are unaffected by these macros.
 */
#ifndef GLGLES_NAMES_H
#define GLGLES_NAMES_H
#define glActiveTexture        glgles_ActiveTexture
#define glAlphaFunc            glgles_AlphaFunc
#define glBegin                glgles_Begin
#define glBindTexture          glgles_BindTexture
#define glBlendFunc            glgles_BlendFunc
#define glClear                glgles_Clear
#define glClearColor           glgles_ClearColor
#define glClearDepth           glgles_ClearDepth
#define glClientActiveTexture  glgles_ClientActiveTexture
#define glColor3f              glgles_Color3f
#define glColor4f              glgles_Color4f
#define glColor4fv             glgles_Color4fv
#define glColor4ub             glgles_Color4ub
#define glColorMask            glgles_ColorMask
#define glColorPointer         glgles_ColorPointer
#define glCopyTexSubImage2D    glgles_CopyTexSubImage2D
#define glDeleteTextures       glgles_DeleteTextures
#define glDepthFunc            glgles_DepthFunc
#define glDepthMask            glgles_DepthMask
#define glDisable              glgles_Disable
#define glDisableClientState   glgles_DisableClientState
#define glDrawArrays           glgles_DrawArrays
#define glDrawElements         glgles_DrawElements
#define glEnable               glgles_Enable
#define glEnableClientState    glgles_EnableClientState
#define glEnd                  glgles_End
#define glFinish               glgles_Finish
#define glFlush                glgles_Flush
#define glFrustum              glgles_Frustum
#define glGenTextures          glgles_GenTextures
#define glGetError             glgles_GetError
#define glGetIntegerv          glgles_GetIntegerv
#define glGetString            glgles_GetString
#define glHint                 glgles_Hint
#define glIsEnabled            glgles_IsEnabled
#define glLineWidth            glgles_LineWidth
#define glLoadIdentity         glgles_LoadIdentity
#define glLoadMatrixf          glgles_LoadMatrixf
#define glMatrixMode           glgles_MatrixMode
#define glMultMatrixf          glgles_MultMatrixf
#define glOrtho                glgles_Ortho
#define glPixelMapfv           glgles_PixelMapfv
#define glPixelStorei          glgles_PixelStorei
#define glPixelTransferi       glgles_PixelTransferi
#define glPopAttrib            glgles_PopAttrib
#define glPopMatrix            glgles_PopMatrix
#define glPushAttrib           glgles_PushAttrib
#define glPushMatrix           glgles_PushMatrix
#define glReadBuffer           glgles_ReadBuffer
#define glReadPixels           glgles_ReadPixels
#define glRotatef              glgles_Rotatef
#define glScalef               glgles_Scalef
#define glScissor              glgles_Scissor
#define glShadeModel           glgles_ShadeModel
#define glTexCoord2f           glgles_TexCoord2f
#define glTexCoord4f           glgles_TexCoord4f
#define glTexCoordPointer      glgles_TexCoordPointer
#define glTexEnvf              glgles_TexEnvf
#define glTexEnvfv             glgles_TexEnvfv
#define glTexEnvi              glgles_TexEnvi
#define glTexImage2D           glgles_TexImage2D
#define glTexParameteri        glgles_TexParameteri
#define glTexSubImage2D        glgles_TexSubImage2D
#define glTranslatef           glgles_Translatef
#define glVertex2f             glgles_Vertex2f
#define glVertex2i             glgles_Vertex2i
#define glVertex3f             glgles_Vertex3f
#define glVertexPointer        glgles_VertexPointer
#define glViewport             glgles_Viewport
#endif

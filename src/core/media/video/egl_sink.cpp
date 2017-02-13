/*
 * Copyright © 2017 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Alfonso Sanchez-Beato <alfonso.sanchez-beato@canonical.com>
 */

#include <core/media/video/egl_sink.h>
#include <core/media/video/socket_types.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <sstream>
#include <thread>
#include <future>
#include <cstring>
#include <unistd.h>

namespace media = core::ubuntu::media;
namespace video = core::ubuntu::media::video;

using namespace std;

struct video::EglSink::Private
{

    static bool receive_buff(int socket, BufferData *data)
    {
        struct msghdr msg{};
        struct iovec io = { .iov_base = &data->meta,
                            .iov_len = sizeof data->meta };
        char c_buffer[256];

        msg.msg_iov = &io;
        msg.msg_iovlen = 1;

        msg.msg_control = c_buffer;
        msg.msg_controllen = sizeof c_buffer;

        if (recvmsg(socket, &msg, 0) < 0) {
            cout << "Failed to receive message\n";
            return false;
        }

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

        memmove(&data->fd, CMSG_DATA(cmsg), sizeof data->fd);

        cout << "Extracted fd " << data->fd << '\n';
        cout << "width    " << data->meta.width << '\n';
        cout << "height   " << data->meta.height << '\n';
        cout << "fourcc 0x" << hex << data->meta.fourcc << dec << '\n';
        cout << "stride   " << data->meta.stride << '\n';
        cout << "offset   " << data->meta.offset << '\n';

        return true;
    }

    static void read_sock_events(const media::Player::PlayerKey key,
                                 int sock_fd,
                                 promise<BufferData>& prom_buff,
                                 core::Signal<void>& frame_available)
    {
        static const char *consumer_socket = "media-consumer";

        struct sockaddr_un local;
        int len;
        BufferData buff_data;

        if (sock_fd == -1) {
            perror("Cannot create buffer consumer socket");
            return;
        }

        ostringstream sock_name_ss;
        sock_name_ss << consumer_socket << key;
        local.sun_family = AF_UNIX;
        local.sun_path[0] = '\0';
        strcpy(local.sun_path + 1, sock_name_ss.str().c_str());
        len = sizeof(local.sun_family) + sock_name_ss.str().length() + 1;
        if (bind(sock_fd, (struct sockaddr *) &local, len) == -1) {
            perror("Cannot bind consumer socket");
            return;
        }

        // Wait for buffer descriptions, pass them to rendering thread
        if (!receive_buff(sock_fd, &buff_data))
            return;

        prom_buff.set_value(buff_data);

        // Now signal frame syncs
        while(true) {
            ssize_t res;
            char c;

            res = recv(sock_fd, &c, sizeof c, 0);
            if (res == -1) {
                perror("while waiting sync");
                return;
            } else if (res == 0) {
                cout << "Socket shutdown\n";
                return;
            }

            frame_available();
        }
    }

    bool find_extension(const string& extensions, const string& ext)
    {
        size_t len_all = extensions.length();
        size_t len = ext.length();
        size_t pos = 0;

        while ((pos = extensions.find(ext, pos)) != string::npos) {
            if (pos + len == len_all || extensions[pos + len] == ' ')
                return true;

            pos = pos + len;
        }

        return false;
    }

    Private(uint32_t gl_texture, const media::Player::PlayerKey key)
        : gl_texture{gl_texture},
          prom_buff{},
          fut_buff{prom_buff.get_future()},
          sock_fd{socket(AF_UNIX, SOCK_DGRAM, 0)},
          sock_thread{read_sock_events, key, sock_fd,
                      ref(prom_buff), ref(frame_available)},
          egl_image{EGL_NO_IMAGE_KHR},
          buf_fd{-1}
    {
        const char *extensions;
        const char *egl_needed[] = {"EGL_KHR_image_base",
                                    "EGL_EXT_image_dma_buf_import"};
        EGLDisplay egl_display = eglGetCurrentDisplay();
        size_t i;

        extensions = eglQueryString (egl_display, EGL_EXTENSIONS);
        if (!extensions)
            throw runtime_error {"Error querying EGL extensions"};

        for (i = 0; i < sizeof(egl_needed)/sizeof(egl_needed[0]); ++i) {
            if (!find_extension(extensions, egl_needed[i])) {
                ostringstream oss;
                oss << egl_needed[i] << " not supported";
                cout << oss.str() << '\n';
                // TODO check why extensions is different from es2_info output
                //throw runtime_error {oss.str().c_str()};
            }
        }

        // TODO this returns a NULL pointer, probably same issue as with eglQueryString
        // extensions = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
        // if (!extensions)
        //     throw runtime_error {"Error querying OpenGL ES extensions"};

        // if (!find_extension(extensions, "GL_OES_EGL_image_external"))
        //     throw runtime_error {"GL_OES_EGL_image_external is not supported"};

        // Import functions from extensions
        _eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
            eglGetProcAddress("eglCreateImageKHR");
        _eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
            eglGetProcAddress("eglDestroyImageKHR");
        _glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");

        if (_eglCreateImageKHR == nullptr || _eglDestroyImageKHR == nullptr ||
            _glEGLImageTargetTexture2DOES == nullptr)
            throw runtime_error {"Error when loading extensions"};
    }

    ~Private()
    {
        if (sock_fd != -1) {
            shutdown(sock_fd, SHUT_RDWR);
            sock_thread.join();
            close(sock_fd);
        }

        if (buf_fd != -1)
            close(buf_fd);

        if (egl_image != EGL_NO_IMAGE_KHR)
            _eglDestroyImageKHR(eglGetCurrentDisplay(), egl_image);
    }

    bool import_buffer(const BufferData *buf_data)
    {
        GLenum err;
        EGLDisplay egl_display = eglGetCurrentDisplay();
        EGLint image_attrs[] = {
            EGL_WIDTH, buf_data->meta.width,
            EGL_HEIGHT, buf_data->meta.height,
            EGL_LINUX_DRM_FOURCC_EXT, buf_data->meta.fourcc,
            EGL_DMA_BUF_PLANE0_FD_EXT, buf_data->fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, buf_data->meta.offset,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, buf_data->meta.stride,
            EGL_NONE
        };

        buf_fd = buf_data->fd;
        egl_image = _eglCreateImageKHR(egl_display, EGL_NO_CONTEXT,
                                       EGL_LINUX_DMA_BUF_EXT, NULL, image_attrs);
        if (egl_image == EGL_NO_IMAGE_KHR) {
            cout << "eglCreateImageKHR error 0x" << hex
                 << eglGetError() << dec << '\n';
            return false;
        }

        // TODO Do this when swapping if we end up importing more than one buffer
        glBindTexture(GL_TEXTURE_2D, gl_texture);
        _glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, egl_image);

        while((err = glGetError()) != GL_NO_ERROR)
            cout << "OpenGL error 0x" << hex << err << dec << '\n';

        cout << "Image successfully imported\n";

        return true;
    }

    uint32_t gl_texture;
    promise<BufferData> prom_buff;
    future<BufferData> fut_buff;
    core::Signal<void> frame_available;
    int sock_fd;
    thread sock_thread;
    EGLImageKHR egl_image;
    int buf_fd;
    PFNEGLCREATEIMAGEKHRPROC _eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC _eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC _glEGLImageTargetTexture2DOES;
};

function<video::Sink::Ptr(uint32_t)>
video::EglSink::factory_for_key(const media::Player::PlayerKey& key)
{
    return [key](uint32_t texture)
    {
        return video::Sink::Ptr{new video::EglSink{texture, key}};
    };
}

video::EglSink::EglSink(uint32_t gl_texture,
                        const media::Player::PlayerKey key)
    : d{new Private{gl_texture, key}}
{
}

video::EglSink::~EglSink()
{
}

const core::Signal<void>& video::EglSink::frame_available() const
{
    return d->frame_available;
}

bool video::EglSink::transformation_matrix(float *matrix) const
{
    // TODO: Can we get orientation on unity8 desktop somehow?
    static const float identity_4x4[] = { 1, 0, 0, 0,
                                          0, 1, 0, 0,
                                          0, 0, 1, 0,
                                          0, 0, 0, 1 };

    memcpy(matrix, identity_4x4, sizeof identity_4x4);
    return true;
}

bool video::EglSink::swap_buffers() const
{
    // First time called, import buffers
    if (d->egl_image == EGL_NO_IMAGE_KHR) {
        BufferData buf_data = d->fut_buff.get();
        if (!d->import_buffer(&buf_data))
            return false;
    }

    // We need to do nothing here, as the only buffer has already been mapped.
    // TODO Change when we implement a buffer queue.

    return true;
}

﻿/***

    Olive - Non-Linear Video Editor
    Copyright (C) 2019  Olive Team

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

#ifndef RENDERTHREAD_H
#define RENDERTHREAD_H

#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>

#include "project/sequence.h"
#include "project/effect.h"

// copied from source code to OCIODisplay
const int LUT3D_EDGE_SIZE = 32;

// copied from source code to OCIODisplay, expanded from 3*LUT3D_EDGE_SIZE*LUT3D_EDGE_SIZE*LUT3D_EDGE_SIZE
const int NUM_3D_ENTRIES = 98304;

class RenderThread : public QThread {
  Q_OBJECT
public:
  RenderThread();
  ~RenderThread();
  void run();

  QMutex* get_texture_mutex();
  const GLuint& get_texture();

  EffectPtr gizmos;
  void paint();
  void start_render(QOpenGLContext* share,
                    SequencePtr s,
                    const QString &save = nullptr,
                    GLvoid *pixels = nullptr,
                    int pixel_linesize = 0,
                    int idivider = 0);
  bool did_texture_fail();
  void cancel();


public slots:
  // cleanup functions
  void delete_ctx();
signals:
  void ready();
private:
  void allocate_texture(GLuint tex, GLuint fbo);

  void set_up_ocio();
  void destroy_ocio();

  // cleanup functions
  void delete_texture();
  void delete_fbo();
  void delete_shader_program();

  GLuint front_buffer1;
  GLuint front_texture1;
  QMutex front_mutex1;

  GLuint front_buffer2;
  GLuint front_texture2;
  QMutex front_mutex2;

  bool front_buffer_switcher;

  QWaitCondition wait_cond_;
  QMutex wait_lock_;

  QWaitCondition main_thread_wait_cond_;
  QMutex main_thread_lock_;

  QOffscreenSurface surface;
  QOpenGLContext* share_ctx;
  QOpenGLContext* ctx;
  QOpenGLShaderProgram* blend_mode_program;
  QOpenGLShaderProgram* premultiply_program;

  GLuint back_buffer_1;
  GLuint back_buffer_2;
  GLuint back_texture_1;
  GLuint back_texture_2;

  float ocio_lut_data[NUM_3D_ENTRIES];
  GLuint ocio_lut_texture;
  QOpenGLShaderProgram* ocio_shader;

  SequencePtr seq;
  int divider;
  int tex_width;
  int tex_height;
  bool queued;
  bool texture_failed;
  bool running;
  QString save_fn;
  GLvoid *pixel_buffer;
  int pixel_buffer_linesize;
};

#endif // RENDERTHREAD_H

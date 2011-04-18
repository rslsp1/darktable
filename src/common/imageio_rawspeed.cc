/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_RAWSPEED
#ifdef _OPENMP
#include <omp.h>
#endif
#include "rawspeed/RawSpeed/StdAfx.h"
#include "rawspeed/RawSpeed/FileReader.h"
#include "rawspeed/RawSpeed/TiffParser.h"
#include "rawspeed/RawSpeed/RawDecoder.h"
#include "rawspeed/RawSpeed/CameraMetaData.h"
#include "rawspeed/RawSpeed/ColorFilterArray.h"

extern "C"
{
#include "imageio.h"
#include "common/imageio_rawspeed.h"
#include "common/exif.h"
#include "common/darktable.h"
#include "common/colorspaces.h"
}

// define this function, it is only declared in rawspeed:
int
rawspeed_get_number_of_processor_cores()
{
#ifdef _OPENMP
  return omp_get_num_procs();
#else
  return 1;
#endif
}

using namespace RawSpeed;

dt_imageio_retval_t dt_imageio_open_rawspeed_sraw(dt_image_t *img, RawImage r);
dt_imageio_retval_t dt_imageio_open_rawspeed_sraw_preview(dt_image_t *img, RawImage r);
static CameraMetaData *meta = NULL;

#if 0
static void
scale_black_white(uint16_t *const buf, const uint16_t black, const uint16_t white, const int width, const int height, const int stride)
{
  const float scale = 65535.0f/(white-black);
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static)
#endif
  for(int j=0; j<height; j++)
  {
    uint16_t *b = buf + j*stride;
    for(int i=0; i<width; i++)
    {
      b[0] = CLAMPS((b[0] - black)*scale, 0, 0xffff);
      b++;
    }
  }
}
#endif

dt_imageio_retval_t
dt_imageio_open_rawspeed(dt_image_t *img, const char *filename)
{
  if(!img->exif_inited)
    (void) dt_exif_read(img, filename);

  char filen[1024];
  snprintf(filen, 1024, "%s", filename);
  FileReader f(filen);

  RawDecoder *d = NULL;
  FileMap* m = NULL;
  try
  {
    if(meta == NULL)
    {
      dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
      if(meta == NULL)
      {
        char datadir[1024], camfile[1024];
        dt_get_datadir(datadir, 1024);
        snprintf(camfile, 1024, "%s/rawspeed/cameras.xml", datadir);
        // never cleaned up (only when dt closes)
        meta = new CameraMetaData(camfile);
      }
      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    }
    try
    {
      m = f.readFile();
    }
    catch (FileIOException e)
    {
      return DT_IMAGEIO_FILE_CORRUPTED;
    }
    TiffParser t(m);
    t.parseData();
    d = t.getDecoder();
    if(!d) return DT_IMAGEIO_FILE_CORRUPTED;
    try
    {
      d->failOnUnknown = true;
      d->checkSupport(meta);

      d->decodeRaw();
      d->decodeMetaData(meta);
      RawImage r = d->mRaw;

      img->filters = 0;
      if( r->subsampling.x > 1 || r->subsampling.y > 1 )
      {
        img->flags &= ~DT_IMAGE_LDR;
        img->flags |= DT_IMAGE_RAW;

        dt_imageio_retval_t ret = dt_imageio_open_rawspeed_sraw(img, r);
        if (d) delete d;
        if (m) delete m;
        return ret;
      }

      // only scale colors for sizeof(uint16_t) per pixel, not sizeof(float)
      // if(r->getDataType() != TYPE_FLOAT32) scale_black_white((uint16_t *)r->getData(), r->blackLevel, r->whitePoint, r->dim.x, r->dim.y, r->pitch/r->getBpp());
      if(r->getDataType() != TYPE_FLOAT32) r->scaleBlackWhite();
      img->bpp = r->getBpp();
      img->filters = r->cfa.getDcrawFilter();
      if(img->filters)
      {
        img->flags &= ~DT_IMAGE_LDR;
        img->flags |= DT_IMAGE_RAW;
        if(r->getDataType() == TYPE_FLOAT32) img->flags |= DT_IMAGE_HDR;
      }

      // also include used override in orient:
      const int orientation = dt_image_orientation(img);
      img->width  = (orientation & 4) ? r->dim.y : r->dim.x;
      img->height = (orientation & 4) ? r->dim.x : r->dim.y;

      if(dt_image_alloc(img, DT_IMAGE_FULL))
      {
        if (d) delete d;
        if (m) delete m;
        return DT_IMAGEIO_CACHE_FULL;
      }
      dt_image_check_buffer(img, DT_IMAGE_FULL, r->dim.x*r->dim.y*r->getBpp());
      dt_imageio_flip_buffers((char *)img->pixels, (char *)r->getData(), r->getBpp(), r->dim.x, r->dim.y, r->dim.x, r->dim.y, r->pitch, orientation);
    }
    catch (RawDecoderException e)
    {
      // printf("failed decoding raw `%s'\n", e.what());
      if (d) delete d;
      if (m) delete m;
      return DT_IMAGEIO_FILE_CORRUPTED;
    }
  }
  catch (CameraMetadataException e)
  {
    // printf("failed meta data `%s'\n", e.what());
    if (d) delete d;
    if (m) delete m;
    return DT_IMAGEIO_FILE_CORRUPTED;
  }
  catch (TiffParserException e)
  {
    // printf("failed decoding tiff `%s'\n", e.what());
    if (d) delete d;
    if (m) delete m;
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  // clean up raw stuff.
  if (d) delete d;
  if (m) delete m;
  dt_image_release(img, DT_IMAGE_FULL, 'w');
  return DT_IMAGEIO_OK;
}

dt_imageio_retval_t
dt_imageio_open_rawspeed_preview(dt_image_t *img, const char *filename)
{
  if(!img->exif_inited)
    (void) dt_exif_read(img, filename);

  char filen[1024];
  snprintf(filen, 1024, "%s", filename);
  FileReader f(filen);

  uint16_t *buf = NULL;
  RawDecoder *d = NULL;
  FileMap* m = NULL;
  try
  {
    if(meta == NULL)
    {
      dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
      if(meta == NULL)
      {
        char datadir[1024], camfile[1024];
        dt_get_datadir(datadir, 1024);
        snprintf(camfile, 1024, "%s/rawspeed/cameras.xml", datadir);
        // never cleaned up (only when dt closes)
        meta = new CameraMetaData(camfile);
      }
      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    }
    try
    {
      m = f.readFile();
    }
    catch (FileIOException e)
    {
      return DT_IMAGEIO_FILE_CORRUPTED;
    }
    TiffParser t(m);
    t.parseData();
    d = t.getDecoder();
    if(!d) return DT_IMAGEIO_FILE_CORRUPTED;
    try
    {
      d->checkSupport(meta);

      d->decodeRaw();
      d->decodeMetaData(meta);
      RawImage r = d->mRaw;

      img->filters = 0;
      if( r->subsampling.x > 1 || r->subsampling.y > 1 )
      {
        img->flags &= ~DT_IMAGE_LDR;
        img->flags |= DT_IMAGE_RAW;

        dt_imageio_retval_t ret = dt_imageio_open_rawspeed_sraw_preview(img, r);
        if (d) delete d;
        if (m) delete m;
        return ret;
      }

      // only scale colors for sizeof(uint16_t) per pixel, not sizeof(float)
      // if(r->getDataType() != TYPE_FLOAT32) scale_black_white((uint16_t *)r->getData(), r->blackLevel, r->whitePoint, r->dim.x, r->dim.y, r->pitch/r->getBpp());
      if(r->getDataType() != TYPE_FLOAT32) r->scaleBlackWhite();
      img->bpp = r->getBpp();
      img->filters = r->cfa.getDcrawFilter();
      if(img->filters)
      {
        img->flags &= ~DT_IMAGE_LDR;
        img->flags |= DT_IMAGE_RAW;
        if(r->getDataType() == TYPE_FLOAT32) img->flags |= DT_IMAGE_HDR;
      }

      // also include used override in orient:
      const int orientation = dt_image_orientation(img);
      img->width  = (orientation & 4) ? r->dim.y : r->dim.x;
      img->height = (orientation & 4) ? r->dim.x : r->dim.y;

      buf = (uint16_t *)dt_alloc_align(16, r->dim.x*r->dim.y*r->getBpp());
      if(!buf)
      {
        if (d) delete d;
        if (m) delete m;
        return DT_IMAGEIO_CACHE_FULL;
      }
      dt_imageio_flip_buffers((char *)buf, (char *)r->getData(), r->getBpp(), r->dim.x, r->dim.y, r->dim.x, r->dim.y, r->pitch, orientation);
    }
    catch (RawDecoderException e)
    {
      if (d) delete d;
      if (m) delete m;
      return DT_IMAGEIO_FILE_CORRUPTED;
    }
  }
  catch (CameraMetadataException e)
  {
    if (d) delete d;
    if (m) delete m;
    return DT_IMAGEIO_FILE_CORRUPTED;
  }
  catch (TiffParserException e)
  {
    if (d) delete d;
    if (m) delete m;
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  dt_imageio_retval_t retv = DT_IMAGEIO_OK;
  if(img->filters)
  {
    img->flags &= ~DT_IMAGE_LDR;
    img->flags |= DT_IMAGE_RAW;
  }
  dt_image_raw_to_preview(img, (float *)buf);

  // clean up raw stuff.
  if (buf) free(buf);
  if (d) delete d;
  if (m) delete m;
  dt_image_release(img, DT_IMAGE_FULL, 'w');
  return retv;
}

dt_imageio_retval_t
dt_imageio_open_rawspeed_sraw(dt_image_t *img, RawImage r)
{
  // this is modelled substantially on dt_imageio_open_tiff

  img->flags &= ~DT_IMAGE_LDR;
  img->flags |= DT_IMAGE_RAW;

  const int orientation = dt_image_orientation(img);
  img->width  = (orientation & 4) ? r->dim.y : r->dim.x;
  img->height = (orientation & 4) ? r->dim.x : r->dim.y;

  int raw_width = r->dim.x;
  int raw_height = r->dim.y;

  // work around 50D bug
  char makermodel[1024];
  dt_colorspaces_get_makermodel(makermodel, 1024, img->exif_maker, img->exif_model);
  bool is_50d = !strncmp(makermodel, "Canon EOS 50D", 13);
  int raw_width_extra = (is_50d && r->subsampling.y == 2) ? 72 : 0;

  if(dt_image_alloc(img, DT_IMAGE_FULL))
    return DT_IMAGEIO_CACHE_FULL;

  dt_image_check_buffer(img, DT_IMAGE_FULL, 4*img->width*img->height*sizeof(float));

  int black = r->blackLevel;
  int white = r->whitePoint;

  ushort16* raw_img = (ushort16*)r->getData();

#if 0
  dt_imageio_flip_buffers_ui16_to_float(img->pixels, raw_img, black, white, 3, raw_width, raw_height,
                                        raw_width, raw_height, raw_width + raw_width_extra, orientation);
#else

  // TODO - OMPize this.
  float scale = 1.0 / (white - black);
  for( int row = 0; row < raw_height; ++row )
    for( int col = 0; col < raw_width; ++col )
      for( int k = 0; k < 3; ++k )
        img->pixels[4 * dt_imageio_write_pos(col, row, raw_width, raw_height, raw_width, raw_height, orientation) + k] = (float)raw_img[row*(raw_width + raw_width_extra)*3 + col*3 + k] * scale;
#endif

  dt_image_release(img, DT_IMAGE_FULL, 'w');
  return DT_IMAGEIO_OK;
}

dt_imageio_retval_t
dt_imageio_open_rawspeed_sraw_preview(dt_image_t *img, RawImage r)
{
  img->flags &= ~DT_IMAGE_LDR;
  img->flags |= DT_IMAGE_RAW;

  const int orientation = dt_image_orientation(img);
  img->width  = (orientation & 4) ? r->dim.y : r->dim.x;
  img->height = (orientation & 4) ? r->dim.x : r->dim.y;

  int raw_width = r->dim.x;
  int raw_height = r->dim.y;

  // work around 50D bug
  char makermodel[1024];
  dt_colorspaces_get_makermodel(makermodel, 1024, img->exif_maker, img->exif_model);
  bool is_50d = !strncmp(makermodel, "Canon EOS 50D", 13);
  int raw_width_extra = (is_50d && r->subsampling.y == 2) ? 72 : 0;

  float *buf = (float*)dt_alloc_align(16, raw_width * raw_height * 4 * sizeof(float) );

  if(!buf)
    return DT_IMAGEIO_CACHE_FULL;

  int black = r->blackLevel;
  int white = r->whitePoint;
  float scale = 1.0 / (white - black);

  ushort16* raw_img = (ushort16*)r->getData();

  // TODO - OMPize this.

  for( int row = 0; row < raw_height; ++row )
    for( int col = 0; col < raw_width; ++col )
      for( int k = 0; k < 3; ++k )
        buf[4 * dt_imageio_write_pos(col, row, raw_width, raw_height, raw_width, raw_height, orientation) + k] = (float)raw_img[row*(raw_width + raw_width_extra)*3 + col*3 + k] * scale;

  dt_image_raw_to_preview(img, buf);

  if(buf) free(buf);
  dt_image_release(img, DT_IMAGE_FULL, 'w');
  return DT_IMAGEIO_OK;
}

#endif

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;

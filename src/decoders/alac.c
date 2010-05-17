#include "alac.h"
#include "../pcm.h"

/********************************************************
 Audio Tools, a module and set of tools for manipulating audio data
 Copyright (C) 2007-2010  Brian Langenberger

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *******************************************************/

int ALACDecoder_init(decoders_ALACDecoder *self,
		     PyObject *args, PyObject *kwds) {
  char *filename;
  static char *kwlist[] = {"filename",
			   "sample_rate",
			   "channels",
			   "channel_mask",
			   "bits_per_sample",
			   "total_frames",
			   "max_samples_per_frame",
			   "history_mult",
			   "initial_history",
			   "kmodifier"};

  self->filename = NULL;
  self->file = NULL;
  self->bitstream = NULL;

  if (!PyArg_ParseTupleAndKeywords(args,kwds,"siiiiiiiii",kwlist,
				   &filename,
				   &(self->sample_rate),
				   &(self->channels),
				   &(self->channel_mask),
				   &(self->bits_per_sample),
				   &(self->total_frames),
				   &(self->max_samples_per_frame),
				   &(self->history_mult),
				   &(self->initial_history),
				   &(self->kmodifier)))
    return -1;

  /*initialize buffer*/
  iaa_init(&(self->samples),
	   self->channels,
	   self->max_samples_per_frame);

  /*initialize wasted-bits buffer, just in case*/
  iaa_init(&(self->wasted_bits_samples),
	   self->channels,
	   self->max_samples_per_frame);

  /*open the alac file*/
  if ((self->file = fopen(filename,"rb")) == NULL) {
    PyErr_SetFromErrnoWithFilename(PyExc_IOError,filename);
    return -1;
  } else {
    self->bitstream = bs_open(self->file);
  }
  self->filename = strdup(filename);

  /*seek to the 'mdat' atom, which contains the ALAC stream*/
  if (ALACDecoder_seek_mdat(self) == ERROR) {
    PyErr_SetString(PyExc_ValueError,"Unable to locate 'mdat' atom in stream");
    return -1;
  }

  return 0;
}

void ALACDecoder_dealloc(decoders_ALACDecoder *self) {
  iaa_free(&(self->samples));
  iaa_free(&(self->wasted_bits_samples));

  if (self->filename != NULL)
    free(self->filename);
  bs_close(self->bitstream); /*this closes self->file also*/

  self->ob_type->tp_free((PyObject*)self);
}

PyObject *ALACDecoder_new(PyTypeObject *type,
			  PyObject *args, PyObject *kwds) {
  decoders_ALACDecoder *self;

  self = (decoders_ALACDecoder *)type->tp_alloc(type, 0);

  return (PyObject *)self;
}

static PyObject *ALACDecoder_sample_rate(decoders_ALACDecoder *self,
					 void *closure) {
  return Py_BuildValue("i",self->sample_rate);
}

static PyObject *ALACDecoder_bits_per_sample(decoders_ALACDecoder *self,
					     void *closure) {
  return Py_BuildValue("i",self->bits_per_sample);
}

static PyObject *ALACDecoder_channels(decoders_ALACDecoder *self,
				      void *closure) {
  return Py_BuildValue("i",self->channels);
}

static PyObject *ALACDecoder_channel_mask(decoders_ALACDecoder *self,
					  void *closure) {
  return Py_BuildValue("i",self->channel_mask);
}


PyObject *ALACDecoder_read(decoders_ALACDecoder* self,
			   PyObject *args) {
  struct alac_frame_header frame_header;
  struct alac_subframe_header *subframe_headers = NULL;
  PyObject *pcm = NULL;
  pcm_FrameList *framelist = NULL;

  int interlacing_shift;
  int interlacing_leftweight;


  struct i_array *channel_data;
  int channel;
  int i,j;

  iaa_reset(&(self->samples));
  frame_header.output_samples = 0;

  if (self->total_frames < 1)
    goto write_frame;

  if (ALACDecoder_read_frame_header(self->bitstream,
				    &frame_header,
				    self->max_samples_per_frame) == ERROR)
    goto error;

  if (frame_header.is_not_compressed) {
    /*uncompressed samples are interlaced between channels*/
    for (i = 0; i < frame_header.output_samples; i++) {
      for (channel = 0; channel < self->channels; channel++) {
	ia_append(iaa_getitem(&(self->samples),channel),
		  read_signed_bits(self->bitstream,
				   self->bits_per_sample));
      }
    }
  } else {
    interlacing_shift = read_bits(self->bitstream,8);
    interlacing_leftweight = read_bits(self->bitstream,8);

    ALACDecoder_print_frame_header(&frame_header);

    /*read the subframe headers*/
    subframe_headers = malloc(sizeof(struct alac_subframe_header) *
			      self->channels);
    for (i = 0; i < self->channels; i++) {
      ALACDecoder_read_subframe_header(self->bitstream,
				       &(subframe_headers[i]));
      ALACDecoder_print_subframe_header(&(subframe_headers[i]));
    }

    /*if there are wasted bits, read a block of interlaced
      wasted-bits samples, each (wasted_bits * 8) large*/
    if (frame_header.wasted_bits > 0) {
      ALACDecoder_read_wasted_bits(self->bitstream,
				   &(self->wasted_bits_samples),
				   frame_header.output_samples,
				   frame_header.channels,
				   frame_header.wasted_bits * 8);
    }

    PyErr_SetString(PyExc_ValueError,"TODO: read residual data");
    goto error;
  }

  /*each frame has a 3 byte '111' signature prior to byte alignment*/
  if (read_bits(self->bitstream,3) != 7) {
    PyErr_SetString(PyExc_ValueError,"invalid signature at end of frame");
    goto error;
  } else {
    byte_align_r(self->bitstream);
  }

  /*transform the contents of self->samples into a pcm.FrameList object*/
 write_frame:
  if ((pcm = PyImport_ImportModule("audiotools.pcm")) == NULL)
    goto error;
  framelist = (pcm_FrameList*)PyObject_CallMethod(pcm,"__blank__",NULL);
  Py_DECREF(pcm);
  framelist->frames = iaa_getitem(&(self->samples),0)->size;
  framelist->channels = self->channels;
  framelist->bits_per_sample = self->bits_per_sample;
  framelist->samples_length = framelist->frames * framelist->channels;
  framelist->samples = realloc(framelist->samples,
			       sizeof(ia_data_t) * framelist->samples_length);

  for (channel = 0; channel < self->channels; channel++) {
    channel_data = iaa_getitem(&(self->samples),channel);
    for (i = channel,j = 0; j < frame_header.output_samples;
  	 i += self->channels,j++)
      framelist->samples[i] = ia_getitem(channel_data,j);
  }

  self->total_frames -= framelist->frames;

  if (subframe_headers != NULL) {
    for (i = 0; i < self->channels; i++)
      ALACDecoder_free_subframe_header(&(subframe_headers[i]));
    free(subframe_headers);
  }
  return (PyObject*)framelist;
 error:
  if (subframe_headers != NULL) {
    for (i = 0; i < self->channels; i++)
      ALACDecoder_free_subframe_header(&(subframe_headers[i]));
    free(subframe_headers);
  }
  return NULL;
}

PyObject *ALACDecoder_close(decoders_ALACDecoder* self,
			    PyObject *args) {
  Py_INCREF(Py_None);
  return Py_None;
}

status ALACDecoder_seek_mdat(decoders_ALACDecoder *self) {
  uint32_t atom_size;
  uint32_t atom_type;
  struct stat file_stat;
  off_t i = 0;

  /*potential race condition here if file changes out from under us*/
  if (stat(self->filename,&file_stat))
    return ERROR;

  while (i < file_stat.st_size) {
    atom_size = read_bits(self->bitstream,32);
    atom_type = read_bits(self->bitstream,32);
    if (atom_type == 0x6D646174)
      return OK;
    fseek(self->file,atom_size - 8,SEEK_CUR);
    i += atom_size;
  }

  return ERROR;
}

status ALACDecoder_read_frame_header(Bitstream *bs,
				     struct alac_frame_header *frame_header,
				     int max_samples_per_frame) {
  frame_header->channels = read_bits(bs,3) + 1;
  read_bits(bs,16); /*nobody seems to know what these are for*/
  frame_header->has_size = read_bits(bs,1);
  frame_header->wasted_bits = read_bits(bs,2);
  frame_header->is_not_compressed = read_bits(bs,1);
  if (frame_header->has_size) {
    /*for when we hit the end of the stream
      and need a non-typical amount of samples*/
    frame_header->output_samples = read_bits(bs,32);
  } else {
    frame_header->output_samples = max_samples_per_frame;
  }

  return OK;
}

status ALACDecoder_read_subframe_header(Bitstream *bs,
					struct alac_subframe_header *subframe_header) {
  int predictor_coef_num;
  int i;

  subframe_header->prediction_type = read_bits(bs,4);
  subframe_header->prediction_quantitization = read_bits(bs,4);
  subframe_header->rice_modifier = read_bits(bs,3);
  predictor_coef_num = read_bits(bs,5);
  ia_init(&(subframe_header->predictor_coef_table),
	  predictor_coef_num);
  for (i = 0; i < predictor_coef_num; i++) {
    ia_append(&(subframe_header->predictor_coef_table),
	      read_signed_bits(bs,16));
  }

  return OK;
}

status ALACDecoder_read_wasted_bits(Bitstream *bs,
				    struct ia_array *wasted_bits_samples,
				    int sample_count,
				    int channels,
				    int wasted_bits_size) {
  int i;
  int channel;

  for (i = 0; i < sample_count; i++) {
    for (channel = 0; channel < channels; channel++) {
      ia_append(iaa_getitem(wasted_bits_samples,channel),
		read_bits(bs,wasted_bits_size));
    }
  }

  return OK;
}

void ALACDecoder_print_frame_header(struct alac_frame_header *frame_header) {
  printf("channels : %d\n",frame_header->channels);
  printf("has_size : %d\n",frame_header->has_size);
  printf("wasted bits : %d\n",frame_header->wasted_bits);
  printf("is_not_compressed : %d\n",frame_header->is_not_compressed);
  printf("output_samples : %d\n",frame_header->output_samples);
}

void ALACDecoder_print_subframe_header(struct alac_subframe_header *subframe_header) {
  printf("prediction type : %d\n",subframe_header->prediction_type);
  printf("prediction quantitization : %d\n",subframe_header->prediction_quantitization);
  printf("rice modifier : %d\n",subframe_header->rice_modifier);
  printf("predictor coefficients : ");
  ia_print(stdout,&(subframe_header->predictor_coef_table));
  printf("\n");
}

/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include <math.h>
#include "sim.h"

#define ResolSinCos 100
static uint16_t LUTSin[ResolSinCos+1];

/* Initialisation function for SIN table values */
void InitSinLUT( void ) {
  for ( int i=0; i<(ResolSinCos+1); i++ ) {
    LUTSin[i] = sin((double)(M_PI*i)/(2*ResolSinCos)) * (1<<14); //Format: Q14

/* linear phase noise model */
void phase_noise(double ts, int16_t * InRe, int16_t * InIm)
{
  static uint64_t i=0;
  int32_t x=0 ,y=0;
  double fd = 300;//0.01*30000
  int16_t SinValue = 0, CosValue= 0;
  double IdxDouble = (double)(i*fd * ts * ResolSinCos * 4);
  int16_t IdxModulo = ((int32_t)(IdxDouble>0 ? IdxDouble+0.5 : IdxDouble-0.5)) % (ResolSinCos*4);
  IdxModulo = IdxModulo<0 ? IdxModulo+ResolSinCos*4 : IdxModulo;

  if(IdxModulo<2*ResolSinCos) {//< 2 check for 1st and 2nd
    if(IdxModulo < ResolSinCos) {// 1st Quadrant
      SinValue = LUTSin[IdxModulo];
      CosValue = LUTSin[ResolSinCos-IdxModulo];
    }
    else {// 2nd Quadrant
      SinValue = LUTSin[2*ResolSinCos-IdxModulo];
      CosValue = -LUTSin[IdxModulo-ResolSinCos];
    }
  }
  else {// 3rd and 4th Quadrant
    if(IdxModulo < 3*ResolSinCos) {// 3rd Quadrant
      SinValue = -LUTSin[IdxModulo-2*ResolSinCos];
      CosValue = -LUTSin[3*ResolSinCos-IdxModulo];
    }
    else {//4th Quadrant
      SinValue = -LUTSin[4*ResolSinCos-IdxModulo];
      CosValue = LUTSin[IdxModulo-3*ResolSinCos];
    }
  }
  x = ( ((int32_t)InRe[0] * CosValue) - ((int32_t)InIm[0] * SinValue ));
  y = ( ((int32_t)InIm[0] * CosValue) + ((int32_t)InRe[0] * SinValue ));
  InRe[0]= (int16_t)(x>>14);
  InIm[0]= (int16_t)(y>>14);
  i++;
}

void add_noise(c16_t **rxdata,
               const double **r_re,
               const double **r_im,
               const double sigma,
               const int length,
               const int slot_offset,
               const double ts,
               const int delay,
               const uint16_t pdu_bit_map,
               const uint16_t ptrs_bit_map,
               const uint8_t nb_antennas_rx)
{
  for (int i = 0; i < length; i++) {
    for (int ap = 0; ap < nb_antennas_rx; ap++) {
      c16_t *rxd = &rxdata[ap][slot_offset + i + delay];
      rxd->r = r_re[ap][i] + sqrt(sigma / 2) * gaussZiggurat(0.0, 1.0); // convert to fixed point
      rxd->i = r_im[ap][i] + sqrt(sigma / 2) * gaussZiggurat(0.0, 1.0);
      /* Add phase noise if enabled */
      if (pdu_bit_map & ptrs_bit_map) {
        phase_noise(ts, &rxdata[ap][slot_offset + i + delay].r, &rxdata[ap][slot_offset + i + delay].i);
      }
    }
  }
}

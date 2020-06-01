#include <stdio.h>
#include <stdint.h>
#include "../../nrLDPCdecoder_defs.h"


void nrLDPC_cnProc_BG2_generator_AVX512(uint16_t Z,int R)
{
  const char *ratestr[3]={"13","23","89"};

  if (R<0 || R>2) {printf("Illegal R %d\n",R); abort();}


 // system("mkdir -p ../ldpc_gen_files");

  char fname[50];
  sprintf(fname,"../ldpc_gen_files/nrLDPC_cnProc_BG2_Z%d_%s_AVX512.c",Z,ratestr[R]);
  FILE *fd=fopen(fname,"w");
  if (fd == NULL) {printf("Cannot create \n");abort();}

  fprintf(fd,"#include <stdint.h>\n");
  fprintf(fd,"#include <immintrin.h>\n");
  fprintf(fd, "#include  \"../include/avx512fintrin.h\"\n");


  fprintf(fd,	"__m512i _mm512_sign_epi16(__m512i a, __m512i b){ \n");     /* Emulate _mm512_sign_epi16() with instructions that exist in the AVX-512 instruction set      */
  fprintf(fd,	"b = _mm512_min_epi16(b, _mm512_set1_epi16(1)); 	 \n" );
  fprintf(fd,	"b = _mm512_max_epi16(b, _mm512_set1_epi16(-1)); 	 \n" );
  fprintf(fd,	" a = _mm512_mullo_epi16(a, b);\n");
  fprintf(fd,	"return a;\n");
  fprintf(fd,  "}\n" );


  fprintf(fd,"void nrLDPC_cnProc_BG2_Z%d_%s_AVX512(int8_t* cnProcBuf,int8_t* cnProcBufRes) {\n",Z,ratestr[R]);

  const uint8_t*  lut_numCnInCnGroups;
  const uint32_t* lut_startAddrCnGroups = lut_startAddrCnGroups_BG2;

  if (R==0)      lut_numCnInCnGroups = lut_numCnInCnGroups_BG2_R15;
  else if (R==1) lut_numCnInCnGroups = lut_numCnInCnGroups_BG2_R13;
  else if (R==2) lut_numCnInCnGroups = lut_numCnInCnGroups_BG2_R23;
  else { printf("aborting, illegal R %d\n",R); fclose(fd);abort();}

  //__m512i* p_cnProcBuf;
  //__m512i* p_cnProcBufRes;

  // Number of CNs in Groups
  uint32_t M;
  uint32_t j;
  uint32_t k;
  // Offset to each bit within a group in terms of 64  Byte
  uint32_t bitOffsetInGroup;

  //__m512i zmm0, min, sgn;
  //__m512i* p_cnProcBufResBit;

  // const __m512i* p_ones   = (__m512i*) ones256_epi8;
  // const __m512i* p_maxLLR = (__m512i*) maxLLR256_epi8;



  // =====================================================================
  // Process group with 3 BNs
  fprintf(fd,"//Process group with 3 BNs\n");
  // LUT with offsets for bits that need to be processed
  // 1. bit proc requires LLRs of 2. and 3. bit, 2.bits of 1. and 3. etc.
  // Offsets are in units of bitOffsetInGroup (1*384/64)=6
    // Offsets are in units of bitOffsetInGroup
    const uint8_t lut_idxCnProcG3[3][2] = {{36,72}, {0,72}, {0,36}};


  fprintf(fd,"                __m512i zmm0, min, sgn,ones,maxLLR;\n");
  fprintf(fd,"                ones   = _mm512_set1_epi8((char)1);\n");
  fprintf(fd,"                maxLLR = _mm512_set1_epi8((char)127);\n");

  if (lut_numCnInCnGroups[0] > 0)
    {
      // Number of groups of 64  CNs for parallel processing
      // Ceil for values not divisible by 64
      M = (lut_numCnInCnGroups[0]*Z + 63)>>5;

      // Set the offset to each bit within a group in terms of 64  Byte
      bitOffsetInGroup = (lut_numCnInCnGroups_BG2_R15[0]*NR_LDPC_ZMAX)>>5;


      // Set pointers to start of group 3
      //p_cnProcBuf    = (__m512i*) &cnProcBuf   [lut_startAddrCnGroups[0]];
      //p_cnProcBufRes = (__m512i*) &cnProcBufRes[lut_startAddrCnGroups[0]];

      // Loop over every BN
      
      for (j=0; j<3; j++)
        {
	  // Set of results pointer to correct BN address
	  //p_cnProcBufResBit = p_cnProcBufRes + (j*bitOffsetInGroup);

	  // Loop over CNs
	  //	  for (i=0; i<M; i++,iprime++)
	  //            {
	  
	  fprintf(fd,"            for (int i=0;i<%d;i+=2) {\n",M);
	  // Abs and sign of 64  CNs (first BN)
	  //                zmm0 = p_cnProcBuf[lut_idxCnProcG3[j][0] + i];
	  fprintf(fd,"                zmm0 = ((__m512i*)cnProcBuf)[%d+i];\n",(lut_startAddrCnGroups[0]>>5)+lut_idxCnProcG3[j][0]);
	  //                sgn  = _mm512_sign_epi16(ones, zmm0);
	  fprintf(fd,"                sgn  = _mm512_sign_epi16(ones, zmm0);\n");
	  //                min  = _mm512_abs_epi8(zmm0);
	  fprintf(fd,"                min  = _mm512_abs_epi8(zmm0);\n");
	  
	  // 32 CNs of second BN
	  //                zmm0 = p_cnProcBuf[lut_idxCnProcG3[j][1] + i];
	  fprintf(fd,"                zmm0 = ((__m512i*)cnProcBuf)[%d+i];\n",(lut_startAddrCnGroups[0]>>5)+lut_idxCnProcG3[j][1]);
	  
	  //                min  = _mm512_min_epu8(min, _mm512_abs_epi8(zmm0));
	  fprintf(fd,"                min  = _mm512_min_epu8(min, _mm512_abs_epi8(zmm0));\n");
	  
	  //                sgn  = _mm512_sign_epi16(sgn, zmm0);
	  fprintf(fd,"                sgn  = _mm512_sign_epi16(sgn, zmm0);\n");
	  
	  // Store result
	  //                min = _mm512_min_epu8(min, maxLLR); // 128 in epi8 is -127
	  fprintf(fd,"                min = _mm512_min_epu8(min, maxLLR);\n");
	  //                *p_cnProcBufResBit = _mm512_sign_epi16(min, sgn);
	  //                p_cnProcBufResBit++;
	  fprintf(fd,"                ((__m512i*)cnProcBufRes)[%d+i] = _mm512_sign_epi16(min, sgn);\n",(lut_startAddrCnGroups[0]>>5)+(j*bitOffsetInGroup));

	  // Abs and sign of 64  CNs (first BN)
	  //                zmm0 = p_cnProcBuf[lut_idxCnProcG3[j][0] + i];
	  fprintf(fd,"                zmm0 = ((__m512i*)cnProcBuf)[%d+i];\n",(lut_startAddrCnGroups[0]>>5)+lut_idxCnProcG3[j][0]+1);
	  //                sgn  = _mm512_sign_epi16(ones, zmm0);
	  fprintf(fd,"                sgn  = _mm512_sign_epi16(ones, zmm0);\n");
	  //                min  = _mm512_abs_epi8(zmm0);
	  fprintf(fd,"                min  = _mm512_abs_epi8(zmm0);\n");
	  
	  // 32 CNs of second BN
	  //                zmm0 = p_cnProcBuf[lut_idxCnProcG3[j][1] + i];
	  fprintf(fd,"                zmm0 = ((__m512i*)cnProcBuf)[%d+i];\n",(lut_startAddrCnGroups[0]>>5)+lut_idxCnProcG3[j][1]+1);
	  
	  //                min  = _mm512_min_epu8(min, _mm512_abs_epi8(zmm0));
	  fprintf(fd,"                min  = _mm512_min_epu8(min, _mm512_abs_epi8(zmm0));\n");
	  
	  //                sgn  = _mm512_sign_epi16(sgn, zmm0);
	  fprintf(fd,"                sgn  = _mm512_sign_epi16(sgn, zmm0);\n");
	  
	  // Store result
	  //                min = _mm512_min_epu8(min, maxLLR); // 128 in epi8 is -127
	  fprintf(fd,"                min = _mm512_min_epu8(min, maxLLR);\n");
	  //                *p_cnProcBufResBit = _mm512_sign_epi16(min, sgn);
	  //                p_cnProcBufResBit++;
	  fprintf(fd,"                ((__m512i*)cnProcBufRes)[%d+i] = _mm512_sign_epi16(min, sgn);\n",(lut_startAddrCnGroups[0]>>5)+(j*bitOffsetInGroup)+1);

	  fprintf(fd,"            }\n");
        }
    }

  // =====================================================================
  // Process group with 4 BNs
  fprintf(fd,"//Process group with 4 BNs\n");
    // Offset is 20*384/64 = 120
    const uint16_t lut_idxCnProcG4[4][3] = {{120,240,360}, {0,240,360}, {0,120,360}, {0,120,240}};

    if (lut_numCnInCnGroups[1] > 0)
    {
      // Number of groups of 64  CNs for parallel processing
      // Ceil for values not divisible by 64
      M = (lut_numCnInCnGroups[1]*Z + 63)>>5;

      // Set the offset to each bit within a group in terms of 64  Byte
      bitOffsetInGroup = (lut_numCnInCnGroups_BG2_R15[1]*NR_LDPC_ZMAX)>>5;


      // Set pointers to start of group 4
      //p_cnProcBuf    = (__m512i*) &cnProcBuf   [lut_startAddrCnGroups[1]];
      //p_cnProcBufRes = (__m512i*) &cnProcBufRes[lut_startAddrCnGroups[1]];

      // Loop over every BN
      for (j=0; j<4; j++)
      {

	  fprintf(fd,"            for (int i=0;i<%d;i++) {\n",M);
	  // Abs and sign of 64  CNs (first BN)
	  //                zmm0 = p_cnProcBuf[lut_idxCnProcG3[j][0] + i];
	  fprintf(fd,"                zmm0 = ((__m512i*)cnProcBuf)[%d+i];\n",(lut_startAddrCnGroups[1]>>5)+lut_idxCnProcG4[j][0]);
	  //                sgn  = _mm512_sign_epi16(ones, zmm0);
	  fprintf(fd,"                sgn  = _mm512_sign_epi16(ones, zmm0);\n");
	  //                min  = _mm512_abs_epi8(zmm0);
	  fprintf(fd,"                min  = _mm512_abs_epi8(zmm0);\n");
	  
	  
	  // Loop over BNs
        for (k=1; k<3; k++)
        {
            fprintf(fd,"                zmm0 = ((__m512i*)cnProcBuf)[%d+i];\n",(lut_startAddrCnGroups[1]>>5)+lut_idxCnProcG4[j][k]);
            //                min  = _mm512_min_epu8(min, _mm512_abs_epi8(zmm0));
            fprintf(fd,"                min  = _mm512_min_epu8(min, _mm512_abs_epi8(zmm0));\n");
            
            //                sgn  = _mm512_sign_epi16(sgn, zmm0);
            fprintf(fd,"                sgn  = _mm512_sign_epi16(sgn, zmm0);\n");
            
        }
	  
        // Store result
        //                min = _mm512_min_epu8(min, maxLLR); // 128 in epi8 is -127
        fprintf(fd,"                min = _mm512_min_epu8(min, maxLLR);\n");
        //                *p_cnProcBufResBit = _mm512_sign_epi16(min, sgn);
            //                p_cnProcBufResBit++;
        fprintf(fd,"                ((__m512i*)cnProcBufRes)[%d+i] = _mm512_sign_epi16(min, sgn);\n",(lut_startAddrCnGroups[1]>>5)+(j*bitOffsetInGroup));
        fprintf(fd,"            }\n");
      }
    }


  // =====================================================================
  // Process group with 5 BNs
  fprintf(fd,"//Process group with 5 BNs\n");
  // Offset is 18*384/64 = 216
    const uint16_t lut_idxCnProcG5[5][4] = {{54,108,162,216}, {0,108,162,216},
                                            {0,54,162,216}, {0,54,108,216}, {0,54,108,162}};


    if (lut_numCnInCnGroups[2] > 0)
    {
      // Number of groups of 64  CNs for parallel processing
      // Ceil for values not divisible by 64
      M = (lut_numCnInCnGroups[2]*Z + 63)>>5;

      // Set the offset to each bit within a group in terms of 64  Byte
      bitOffsetInGroup = (lut_numCnInCnGroups_BG2_R15[2]*NR_LDPC_ZMAX)>>5;

      // Loop over every BN
      
      for (j=0; j<5; j++)
	  {
	   // Set of results pointer to correct BN address
	   //p_cnProcBufResBit = p_cnProcBufRes + (j*bitOffsetInGroup);

	   // Loop over CNs
	 
        fprintf(fd,"            for (int i=0;i<%d;i++) {\n",M);
        // Abs and sign of 64  CNs (first BN)
        //                zmm0 = p_cnProcBuf[lut_idxCnProcG3[j][0] + i];
        fprintf(fd,"                zmm0 = ((__m512i*)cnProcBuf)[%d+i];\n",(lut_startAddrCnGroups[2]>>5)+lut_idxCnProcG5[j][0]);
        //                sgn  = _mm512_sign_epi16(ones, zmm0);
        fprintf(fd,"                sgn  = _mm512_sign_epi16(ones, zmm0);\n");
        //                min  = _mm512_abs_epi8(zmm0);
        fprintf(fd,"                min  = _mm512_abs_epi8(zmm0);\n");
	  
	  
	    // Loop over BNs
	    for (k=1; k<4; k++)
	    {
	      fprintf(fd,"                zmm0 = ((__m512i*)cnProcBuf)[%d+i];\n",(lut_startAddrCnGroups[2]>>5)+lut_idxCnProcG5[j][k]);
	      
	      //                min  = _mm512_min_epu8(min, _mm512_abs_epi8(zmm0));
	      fprintf(fd,"                min  = _mm512_min_epu8(min, _mm512_abs_epi8(zmm0));\n");
	      
	      //                sgn  = _mm512_sign_epi16(sgn, zmm0);
	      fprintf(fd,"                sgn  = _mm512_sign_epi16(sgn, zmm0);\n");
	    }
	  
        // Store result
        //                min = _mm512_min_epu8(min, maxLLR); // 128 in epi8 is -127
        fprintf(fd,"                min = _mm512_min_epu8(min, maxLLR);\n");
        //                *p_cnProcBufResBit = _mm512_sign_epi16(min, sgn);
        //                p_cnProcBufResBit++;
        fprintf(fd,"                ((__m512i*)cnProcBufRes)[%d+i] = _mm512_sign_epi16(min, sgn);\n",(lut_startAddrCnGroups[2]>>5)+(j*bitOffsetInGroup));
        fprintf(fd,"           }\n");
        }
    }

  // =====================================================================
  // Process group with 6 BNs
    fprintf(fd,"//Process group with 6 BNs\n");
    // Offset is 3*384/64 = 18
    const uint16_t lut_idxCnProcG6[6][5] = {{18,36,54,72,90}, {0,36,54,72,90},
                                            {0,18,54,72,90}, {0,18,36,72,90},
                                            {0,18,36,54,90}, {0,18,36,54,72}};


    if (lut_numCnInCnGroups[3] > 0)
    {
      // Number of groups of 64  CNs for parallel processing
      // Ceil for values not divisible by 64
      M = (lut_numCnInCnGroups[3]*Z + 63)>>5;

      // Set the offset to each bit within a group in terms of 64  Byte
      bitOffsetInGroup = (lut_numCnInCnGroups_BG2_R15[3]*NR_LDPC_ZMAX)>>5;


      // Set pointers to start of group 4
      //p_cnProcBuf    = (__m512i*) &cnProcBuf   [lut_startAddrCnGroups[1]];
      //p_cnProcBufRes = (__m512i*) &cnProcBufRes[lut_startAddrCnGroups[1]];

      // Loop over every BN
      
        for (j=0; j<6; j++)
        {
        // Set of results pointer to correct BN address
        //p_cnProcBufResBit = p_cnProcBufRes + (j*bitOffsetInGroup);

        // Loop over CNs
  
        fprintf(fd,"            for (int i=0;i<%d;i++) {\n",M);
        // Abs and sign of 64  CNs (first BN)
        //                zmm0 = p_cnProcBuf[lut_idxCnProcG3[j][0] + i];
        fprintf(fd,"                zmm0 = ((__m512i*)cnProcBuf)[%d+i];\n",(lut_startAddrCnGroups[3]>>5)+lut_idxCnProcG6[j][0]);
        //                sgn  = _mm512_sign_epi16(ones, zmm0);
        fprintf(fd,"                sgn  = _mm512_sign_epi16(ones, zmm0);\n");
        //                min  = _mm512_abs_epi8(zmm0);
        fprintf(fd,"                min  = _mm512_abs_epi8(zmm0);\n");
        
	  
        // Loop over BNs
        for (k=1; k<5; k++)
        {
            fprintf(fd,"                zmm0 = ((__m512i*)cnProcBuf)[%d+i];\n",(lut_startAddrCnGroups[3]>>5)+lut_idxCnProcG6[j][k]);
            
            //                min  = _mm512_min_epu8(min, _mm512_abs_epi8(zmm0));
            fprintf(fd,"                min  = _mm512_min_epu8(min, _mm512_abs_epi8(zmm0));\n");
            
            //                sgn  = _mm512_sign_epi16(sgn, zmm0);
            fprintf(fd,"                sgn  = _mm512_sign_epi16(sgn, zmm0);\n");
	    }
	  
        // Store result
        //                min = _mm512_min_epu8(min, maxLLR); // 128 in epi8 is -127
	    fprintf(fd,"                min = _mm512_min_epu8(min, maxLLR);\n");
	    //                *p_cnProcBufResBit = _mm512_sign_epi16(min, sgn);
	    //                p_cnProcBufResBit++;
        fprintf(fd,"                ((__m512i*)cnProcBufRes)[%d+i] = _mm512_sign_epi16(min, sgn);\n",(lut_startAddrCnGroups[3]>>5)+(j*bitOffsetInGroup));
        fprintf(fd,"            }\n");
        }
    }


  // =====================================================================
  // Process group with 8 BNs
  fprintf(fd,"//Process group with 8 BNs\n");

    // Offset is 2*384/64 = 12
    const uint8_t lut_idxCnProcG8[8][7] = {{12,24,36,48,60,72,84}, {0,24,36,48,60,72,84},
                                           {0,12,36,48,60,72,84}, {0,12,24,48,60,72,84},
                                           {0,12,24,36,60,72,84}, {0,12,24,36,48,72,84},
                                           {0,12,24,36,48,60,84}, {0,12,24,36,48,60,72}};


    if (lut_numCnInCnGroups[5] > 0)
    {
      // Number of groups of 64  CNs for parallel processing
      // Ceil for values not divisible by 64
      M = (lut_numCnInCnGroups[5]*Z + 63)>>5;

      // Set the offset to each bit within a group in terms of 64  Byte
      bitOffsetInGroup = (lut_numCnInCnGroups_BG2_R15[5]*NR_LDPC_ZMAX)>>5;


      // Set pointers to start of group 4
      //p_cnProcBuf    = (__m512i*) &cnProcBuf   [lut_startAddrCnGroups[1]];
      //p_cnProcBufRes = (__m512i*) &cnProcBufRes[lut_startAddrCnGroups[1]];

      // Loop over every BN
     
        for (j=0; j<8; j++)
        {
        // Set of results pointer to correct BN address
        //p_cnProcBufResBit = p_cnProcBufRes + (j*bitOffsetInGroup);

        // Loop over CNs

        fprintf(fd,"            for (int i=0;i<%d;i++) {\n",M);
        // Abs and sign of 64  CNs (first BN)
        //                zmm0 = p_cnProcBuf[lut_idxCnProcG3[j][0] + i];
        fprintf(fd,"                zmm0 = ((__m512i*)cnProcBuf)[%d+i];\n",(lut_startAddrCnGroups[5]>>5)+lut_idxCnProcG8[j][0]);
        //                sgn  = _mm512_sign_epi16(ones, zmm0);
        fprintf(fd,"                sgn  = _mm512_sign_epi16(ones, zmm0);\n");
        //                min  = _mm512_abs_epi8(zmm0);
        fprintf(fd,"                min  = _mm512_abs_epi8(zmm0);\n");
	  
	  
	        // Loop over BNs
	        for (k=1; k<7; k++)
	        {
	        fprintf(fd,"                zmm0 = ((__m512i*)cnProcBuf)[%d+i];\n",(lut_startAddrCnGroups[5]>>5)+lut_idxCnProcG8[j][k]);
	      
	        //                min  = _mm512_min_epu8(min, _mm512_abs_epi8(zmm0));
	        fprintf(fd,"                min  = _mm512_min_epu8(min, _mm512_abs_epi8(zmm0));\n");
	      
	        //                sgn  = _mm512_sign_epi16(sgn, zmm0);
	        fprintf(fd,"                sgn  = _mm512_sign_epi16(sgn, zmm0);\n");
	        }
	  
            // Store result
            //                min = _mm512_min_epu8(min, maxLLR); // 128 in epi8 is -127
            fprintf(fd,"                min = _mm512_min_epu8(min, maxLLR);\n");
            //                *p_cnProcBufResBit = _mm512_sign_epi16(min, sgn);
            //                p_cnProcBufResBit++;
            fprintf(fd,"                ((__m512i*)cnProcBufRes)[%d+i] = _mm512_sign_epi16(min, sgn);\n",(lut_startAddrCnGroups[5]>>5)+(j*bitOffsetInGroup));
            fprintf(fd,"              }\n");
        }
    }

  
  // =====================================================================
  // Process group with 10 BNs
  fprintf(fd,"//Process group with 10 BNs\n");
     // Offset is 2*384/64 = 12
    const uint8_t lut_idxCnProcG10[10][9] = {{12,24,36,48,60,72,84,96,108}, {0,24,36,48,60,72,84,96,108},
                                             {0,12,36,48,60,72,84,96,108}, {0,12,24,48,60,72,84,96,108},
                                             {0,12,24,36,60,72,84,96,108}, {0,12,24,36,48,72,84,96,108},
                                             {0,12,24,36,48,60,84,96,108}, {0,12,24,36,48,60,72,96,108},
                                             {0,12,24,36,48,60,72,84,108}, {0,12,24,36,48,60,144,84,96}};





  if (lut_numCnInCnGroups[7] > 0)
    {
      // Number of groups of 64  CNs for parallel processing
      // Ceil for values not divisible by 64
      M = (lut_numCnInCnGroups[7]*Z + 63)>>5;

      // Set the offset to each bit within a group in terms of 64  Byte
      bitOffsetInGroup = (lut_numCnInCnGroups_BG2_R15[7]*NR_LDPC_ZMAX)>>5;


      // Set pointers to start of group 10
      //p_cnProcBuf    = (__m512i*) &cnProcBuf   [lut_startAddrCnGroups[1]];
      //p_cnProcBufRes = (__m512i*) &cnProcBufRes[lut_startAddrCnGroups[1]];

      // Loop over every BN
     
        for (j=0; j<10; j++)
        {
        // Set of results pointer to correct BN address
        //p_cnProcBufResBit = p_cnProcBufRes + (j*bitOffsetInGroup);

        // Loop over CNs
        
        fprintf(fd,"            for (int i=0;i<%d;i++) {\n",M);
        // Abs and sign of 64  CNs (first BN)
        //                zmm0 = p_cnProcBuf[lut_idxCnProcG3[j][0] + i];
        fprintf(fd,"                zmm0 = ((__m512i*)cnProcBuf)[%d+i];\n",(lut_startAddrCnGroups[7]>>5)+lut_idxCnProcG10[j][0]);
        //                sgn  = _mm512_sign_epi16(ones, zmm0);
        fprintf(fd,"                sgn  = _mm512_sign_epi16(ones, zmm0);\n");
        //                min  = _mm512_abs_epi8(zmm0);
        fprintf(fd,"                min  = _mm512_abs_epi8(zmm0);\n");
        
	  
	    // Loop over BNs
            for (k=1; k<9; k++)
            {
            fprintf(fd,"                zmm0 = ((__m512i*)cnProcBuf)[%d+i];\n",(lut_startAddrCnGroups[7]>>5)+lut_idxCnProcG10[j][k]);
            
            //                min  = _mm512_min_epu8(min, _mm512_abs_epi8(zmm0));
            fprintf(fd,"                min  = _mm512_min_epu8(min, _mm512_abs_epi8(zmm0));\n");
            
            //                sgn  = _mm512_sign_epi16(sgn, zmm0);
            fprintf(fd,"                sgn  = _mm512_sign_epi16(sgn, zmm0);\n");
            }
	  
            // Store result
            //                min = _mm512_min_epu8(min, maxLLR); // 128 in epi8 is -127
            fprintf(fd,"                min = _mm512_min_epu8(min, maxLLR);\n");
            //                *p_cnProcBufResBit = _mm512_sign_epi16(min, sgn);
            //                p_cnProcBufResBit++;
            fprintf(fd,"                ((__m512i*)cnProcBufRes)[%d+i] = _mm512_sign_epi16(min, sgn);\n",(lut_startAddrCnGroups[7]>>5)+(j*bitOffsetInGroup));
            fprintf(fd,"            }\n");
        }
    }



  fprintf(fd,"}\n");
  fclose(fd);
}//end of the function  nrLDPC_cnProc_BG2

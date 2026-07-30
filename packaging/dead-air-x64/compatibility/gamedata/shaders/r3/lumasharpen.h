/* --- Settings --- */

// -- Sharpening --
#define sharp_clamp    0.035  //[0.000 to 1.000] Limits maximum amount of sharpening a pixel recieves - Default is 0.035

// -- Advanced sharpening settings --
#define offset_bias 1.0  //[0.0 to 6.0] Offset bias adjusts the radius of the sampling pattern.


#define px (1/screen_res.x)
#define py (1/screen_res.y)

   /*-----------------------------------------------------------.
  /                      Developer settings                     /
  '-----------------------------------------------------------*/
#define CoefLuma float3(0.2126, 0.7152, 0.0722)      // BT.709 & sRBG luma coefficient (Monitors and HD Television)
//#define CoefLuma float3(0.299, 0.587, 0.114)       // BT.601 luma coefficient (SD Television)
//#define CoefLuma float3(1.0/3.0, 1.0/3.0, 1.0/3.0) // Equal weight coefficient

   /*-----------------------------------------------------------.
  /                          Main code                          /
  '-----------------------------------------------------------*/

float3 LumaSharpenPass(float3 inputcolor, float3 originalColor, float2 tex, float sharp_strength)
{
  float3 ori = originalColor;

  // -- Combining the strength and luma multipliers --
	float3 sharp_strength_luma = (CoefLuma * sharp_strength); //I'll be combining even more multipliers with it later on

    float3 blur_ori = s_image.SampleLevel(smp_nofilter, float3(tex + float2(px,-py) * 0.5 * offset_bias,0),0).rgb; // South East
    blur_ori += s_image.SampleLevel(smp_nofilter, float3(tex + float2(-px,-py) * 0.5 * offset_bias,0),0).rgb;  // South West
    blur_ori += s_image.SampleLevel(smp_nofilter, float3(tex + float2(px,py) * 0.5 * offset_bias,0),0).rgb; // North East
    blur_ori += s_image.SampleLevel(smp_nofilter, float3(tex + float2(-px,py) * 0.5 * offset_bias,0),0).rgb; // North West

    blur_ori *= 0.25;  // ( /= 4) Divide by the number of texture fetches

   /*-----------------------------------------------------------.
  /                            Sharpen                          /
  '-----------------------------------------------------------*/

  // -- Calculate the sharpening --
  float3 sharp = ori - blur_ori;  //Subtracting the blurred image from the original image

  #if 0 //New experimental limiter .. not yet finished
    float sharp_luma = dot(sharp, sharp_strength_luma); //Calculate the luma
    sharp_luma = (abs(sharp_luma)*8.0) * exp(1.0-(abs(sharp_luma)*8.0)) * sign(sharp_luma) / 16.0; //I should probably move the strength modifier here

  #elif 0 //SweetFX 1.4 code
    // -- Adjust strength of the sharpening --
    float sharp_luma = dot(sharp, sharp_strength_luma); //Calculate the luma and adjust the strength

    // -- Clamping the maximum amount of sharpening to prevent halo artifacts --
    sharp_luma = clamp(sharp_luma, -sharp_clamp, sharp_clamp);  //TODO Try a curve function instead of a clamp

  #else //SweetFX 1.5.1 code
    // -- Adjust strength of the sharpening and clamp it--
    float4 sharp_strength_luma_clamp = float4(sharp_strength_luma * (0.5 / sharp_clamp),0.5); //Roll part of the clamp into the dot

    //sharp_luma = saturate((0.5 / sharp_clamp) * sharp_luma + 0.5); //scale up and clamp
    float sharp_luma = saturate(dot(float4(sharp,1.0), sharp_strength_luma_clamp)); //Calculate the luma, adjust the strength, scale up and clamp
    sharp_luma = (sharp_clamp * 2.0) * sharp_luma - sharp_clamp; //scale down
  #endif

  // -- Combining the values to get the final sharpened pixel	--
  //float4 done = ori + sharp_luma;    // Add the sharpening to the original.
  inputcolor.rgb = inputcolor.rgb + sharp_luma;    // Add the sharpening to the input color.

   /*-----------------------------------------------------------.
  /                     Returning the output                    /
  '-----------------------------------------------------------*/
  #if show_sharpen == 1
    //inputcolor.rgb = abs(sharp * 4.0);
    inputcolor.rgb = saturate(0.5 + (sharp_luma * 4)).rrr;
  #endif

  return saturate(inputcolor);

}

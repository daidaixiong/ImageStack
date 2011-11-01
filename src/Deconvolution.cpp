#include "main.h"
#include "Arithmetic.h"
#include "Color.h"
#include "Complex.h"
#include "Deconvolution.h"
#include "DFT.h"
#include "File.h"
#include "GaussTransform.h"
#include "Geometry.h"
#include "KernelEstimation.h"
#include "header.h"

#define FourierTransform(X) (FFT::apply(X, true, true, false))
#define InverseFourierTransform(X) (IFFT::apply(X, true, true, false))

void Deconvolution::help() {
  pprintf("-deconvolution will deconvolve an image with the kernel in the stack.\n"
	  " This operation takes the name of the deconvolution method as a single\n"
	  " argument, plus any optional arguments that the method may require.\n"
	  " Currently supported are \"cho\" (Cho and Lee, 2009) and \"shan\" \n"
	  " (Shan et al, 2008).\n"
	  "\n"
	  "Usage: ImageStack -load blurred -load kernel -deconvolution cho\n"
	  "Usage: ImageStack -load blurred -load kernel -deconvolution shan\n");

}

void Deconvolution::parse(vector<string> args) {
  assert(args.size() == 1, "-deconvolution takes exactly one argument.\n");
  Window kernel = stack(0);
  Window im = stack(1);
  if (args[0] == "cho")
    push(applyCho2009(im, kernel));
  else if (args[0] == "shan")
    push(applyShan2008(im, kernel));
  else
    panic("Unknown method %s\n", args[0].c_str());
}

Image Deconvolution::applyShan2008(Window B, Window K) {
#ifdef NO_FFTW
  panic("FFTW library has not been linked. Please recompile with proper flags.\n");
  return Image(1,1,1,1);
#else
  assert(K.channels == 1 && K.frames == 1 && B.frames == 1,
	 "The kernel must be single-channel, and both the kernel and blurred\n"
	 "image must be single-framed.\n");
  assert(K.width % 2 == 1 && K.height % 2 ==1,
	 "The kernel dimensions must be odd.\n");

  // Prepare constants and images.
  Image Bgray = (B.channels == 3) ? ColorConvert::apply(B, "rgb", "y") : B;
  Image B_large = applyPadding(Bgray);
  Image K_large = KernelEstimation::EnlargeKernel(K, B_large.width, B_large.height);
  Image smoothness_map;
  const int x_padding = (B_large.width - B.width) / 2;
  const int y_padding = (B_large.height - B.height) / 2;

  // Compute the smoothness map.
  {
    Image filter_x(K.width, 1, 1, 1);
    Image filter_y(1, K.height, 1, 1);
    Offset::apply(filter_x, 1.f / K.width);
    Offset::apply(filter_y, 1.f / K.height);
    if (K.width % 2 == 0) filter_x = Crop::apply(filter_x, 0, 0, K.width+1, 1);
    if (K.height %2 == 0) filter_y = Crop::apply(filter_y, 0, 0, 1, K.height+1);
    Image tmp = Convolve::apply(Convolve::apply(Bgray, filter_x, Convolve::Clamp),
				     filter_y, Convolve::Clamp);
    tmp = Multiply::apply(tmp, tmp, Multiply::Elementwise);
    smoothness_map = Bgray.copy();
    smoothness_map = Multiply::apply(smoothness_map, smoothness_map, Multiply::Elementwise);
    smoothness_map = Convolve::apply(Convolve::apply(smoothness_map, filter_x, Convolve::Clamp),
			  filter_y, Convolve::Clamp);
    Subtract::apply(smoothness_map, tmp);
    Scale::apply(smoothness_map, -1.f);

    Threshold::apply(smoothness_map, -25.0f / (256.f * 256.f));
    smoothness_map = Crop::apply(smoothness_map, -x_padding, -y_padding, 0, B_large.width, B_large.height, 1);
  }

  // Prepare Fourier domain stuff.
  FourierTransform(K_large); // K_large = F(K).
  Image FK2 = K_large.copy(); // FK2 = F(K).
  ComplexConjugate::apply(K_large); // K_large = F(K)^T.
  ComplexMultiply::apply(FK2, K_large, false); // FK2 = |F(K)|^2.
  B_large = RealComplex::apply(B_large);
  FourierTransform(B_large);

  // sum w_i | K * (deriv_i L) - (deriv_i I) | ^ 2 
  //   + gamma |Psi_x - deriv_x L|^2 + |Psi_y - deriv_y L|^2
  //        (Psi_x,Psi_y are redundant variables to follow deriv_x L, deriv_y L)
  //   + lambda_2 | Psi_x - deriv_x I|^2 + |Psi_y - deriv_y I|^2, masked by smoothness-Map
  //   + lambda_1 | non-linear prior on Psi_x, Psi_y |
  float lambda_1 = 0.1f, lambda_2 = 15.f;

  Image numerator_base(B_large.width, B_large.height, 1, 2);
  Image denominator_base(B_large.width, B_large.height, 1, 2);

  Image FDeriv[6];
  for (int i = 0; i <= 5; i++) {
    float w_i;
    FDeriv[i] = Image(B_large.width, B_large.height, 1, 2);
    switch (i) {
    case 0: // Original
      w_i = 50.f;
      FDeriv[i](0, 0, 0)[0] = 1.f; break;
    case 1: // dx
      w_i = 25.f;
      FDeriv[i](0, 0, 0)[0] = -1.f;
      FDeriv[i](1, 0, 0)[0] = 1.f; break;
    case 2: // dxx
      w_i = 12.5f;
      FDeriv[i](0, 0, 0)[0] = 1.f;
      FDeriv[i](1, 0, 0)[0] = -2.f;
      FDeriv[i](2, 0, 0)[0] = 1.f; break;
    case 3: // dy
      w_i = 25.f;
      FDeriv[i](0, 0, 0)[0] = -1.f;
      FDeriv[i](0, 1, 0)[0] = 1.f; break;
    case 4: // dyy
      w_i = 12.5f;
      FDeriv[i](0, 0, 0)[0] = 1.f;
      FDeriv[i](0, 1, 0)[0] = -2.f;
      FDeriv[i](0, 2, 0)[0] = 1.f; break;
    case 5: // dxy
      w_i = 12.5f;
      FDeriv[i](0, 0, 0)[0] = 1.f;
      FDeriv[i](1, 0, 0)[0] = -1;
      FDeriv[i](0, 1, 0)[0] = -1;
      FDeriv[i](1, 1, 0)[0] = 1; break;
    }
    FourierTransform(FDeriv[i]);
    Image tmp = FDeriv[i].copy();
    ComplexConjugate::apply(FDeriv[i]); // FDeriv[i] = F(deriv_i)^T
    ComplexMultiply::apply(tmp, FDeriv[i], false); // tmp = |F(deriv_i)|^2
    Image tmq = tmp.copy();
    ComplexMultiply::apply(tmp, FK2, false); // tmp = |F(K)|^2 |F(deriv_i)|^2
    ComplexMultiply::apply(tmq, K_large, false); // tmq = F(K)^T |F(deriv_i)|^2
    ComplexMultiply::apply(tmq, B_large, false); // tmq = F(K)^T |F(deriv_i)|^2 F(I)
    Add::apply(denominator_base, tmp, w_i);
    Add::apply(numerator_base, tmq, w_i);
  }

  Image dIdx = Convolve::apply(B_large, Crop::apply(FDeriv[1], -1, 0, 3, 1), Convolve::Wrap);
  Image dIdy = Convolve::apply(B_large, Crop::apply(FDeriv[3], 0, -1, 1, 3), Convolve::Wrap);
  Image L, FPsi_x, FPsi_y;
  Image Psi_x(B_large.width, B_large.height, 1, 2);
  Image Psi_y(B_large.width, B_large.height, 1, 2);
  float gamma = 2.0f;
  const int MAX_ITERATION = 2;

  // Non-linear prior for gradient (for 8-bit int pixels)
  float k = 2.7f;
  float a = 0.00061f;
  float b = 5.0f;
  float lt = 1.85263f;
  k *= 255.f; a *= 255.f * 255.f; lt /= 255.f; // adjustment for floating point

  for (int iterations = 1; iterations <= MAX_ITERATION; iterations++) {
    printf(" Starting iteration %d of %d\n", iterations, MAX_ITERATION);
    /******************************************/
    /* Optimize over Psi                      */
    /******************************************/
    // Fixing L makes the objective the following:
    //   gamma |Psi_x - deriv_x L|^2 + |Psi_y - deriv_y L|^2
    //   + lambda_2 | Psi_x - deriv_x I|^2 + |Psi_y - deriv_y I|^2, masked by smoothness-Map
    //   + lambda_1 | non-linear prior on Psi_x, Psi_y |
    // This can be done in the spatial domain entirely component-wise,
    // also independent for x and y.
    //  2 gamma (Psi_x - deriv_x L) + 2 lambda_2 (Psi_x - deriv_x I) .* mask
    //   + lambda_1 (non-linear prior on Psi_x)' = 0.
    float ans_x, ans_y, tmp, fscore, tmpscore;
    bool fscore_valid;
    Image dLdx = Convolve::apply(L, Crop::apply(FDeriv[1], -1, 0, 3, 1), Convolve::Wrap);
    Image dLdy = Convolve::apply(L, Crop::apply(FDeriv[3], 0, -1, 1, 3), Convolve::Wrap);
    for (int y = 0; y < B_large.height; y++) {
      for (int x = 0; x < B_large.width; x++) {
	fscore_valid = false;
	// 1) Quadratic region:
	//  2 gamma (Psi_x - deriv_x L) + 2 lambda_2 (Psi_x - deriv_x I) .* mask
	//   + lambda_1 (-2a Psi_x) = 0.
	// Rearranging gives:
	//     (gamma + lambda_2 - a * lambda_1) psi_x = (gamma deriv_x L + lambda_2 deriv_x I .* mask)
	tmp = (gamma * dLdx(x, y)[0] + lambda_2 * dIdx(x, y)[0] * smoothness_map(x, y)[0])
	  / (gamma + lambda_2 - a * lambda_1);
	tmpscore = gamma * (tmp - dLdx(x, y)[0]) * (tmp - dLdx(x, y)[0])
	  + lambda_2 * (tmp - dIdx(x, y)[0]) * (tmp - dIdx(x, y)[0]) * smoothness_map(x, y)[0]
	  + lambda_1 * (-(a*tmp*tmp + b));
	if (fabs(tmp) > lt && (!fscore_valid || fscore > tmpscore)) {
	  fscore = tmpscore; ans_x = tmp; fscore_valid = true;
	}
	// 2) Positive linear region:
	//  2 gamma (Psi_x - deriv_x L) + 2 lambda_2 (Psi_x - deriv_x I) .* mask
	//   + lambda_1 (-k) = 0.
	// Rearranging gives:
	//     (gamma + lambda_2) psi_x = (gamma deriv_x L + lambda_2 deriv_x I .* mask + lambda_1 k)
	tmp = (gamma * dLdx(x, y)[0] + lambda_2 * dIdx(x, y)[0] * smoothness_map(x, y)[0] + lambda_1 * k)
	  / (gamma + lambda_2);
	tmpscore = gamma * (tmp - dLdx(x, y)[0]) * (tmp - dLdx(x, y)[0])
	  + lambda_2 * (tmp - dIdx(x, y)[0]) * (tmp - dIdx(x, y)[0]) * smoothness_map(x, y)[0]
	  + lambda_1 * (-k * tmp);
	if (tmp >= 0 && tmp <= lt && (!fscore_valid || fscore > tmpscore)) {
	  fscore = tmpscore; ans_x = tmp; fscore_valid = true;
	}
	// 3) Negative linear region:
	//  2 gamma (Psi_x - deriv_x L) + 2 lambda_2 (Psi_x - deriv_x I) .* mask
	//   + lambda_1 (k) = 0.
	// Rearranging gives:
	//     (gamma + lambda_2) psi_x = (gamma deriv_x L + lambda_2 deriv_x I .* mask - lambda_1 k)
	tmp = (gamma * dLdx(x, y)[0] + lambda_2 * dIdx(x, y)[0] * smoothness_map(x, y)[0] - lambda_1 * k)
	  / (gamma + lambda_2);
	tmpscore = gamma * (tmp - dLdx(x, y)[0]) * (tmp - dLdx(x, y)[0])
	  + lambda_2 * (tmp - dIdx(x, y)[0]) * (tmp - dIdx(x, y)[0]) * smoothness_map(x, y)[0]
	  + lambda_1 * (k * tmp);
	if (tmp >= 0 && tmp <= lt && (!fscore_valid || fscore > tmpscore)) {
	  fscore = tmpscore; ans_x = tmp; fscore_valid = true;
	} 
	// 4) Zero
	tmp = 0.f;
	tmpscore = gamma * (tmp - dLdx(x, y)[0]) * (tmp - dLdx(x, y)[0])
	  + lambda_2 * (tmp - dIdx(x, y)[0]) * (tmp - dIdx(x, y)[0]) * smoothness_map(x, y)[0];
	if (!fscore_valid || fscore > tmpscore) {
	  fscore = tmpscore; ans_x = tmp; fscore_valid = true;
	}
	// 5) lt
	tmp = lt;
	tmpscore = gamma * (tmp - dLdx(x, y)[0]) * (tmp - dLdx(x, y)[0])
	  + lambda_2 * (tmp - dIdx(x, y)[0]) * (tmp - dIdx(x, y)[0]) * smoothness_map(x, y)[0]
	  - lambda_1 * (-k * lt);
	if (!fscore_valid || fscore > tmpscore) {
	  fscore = tmpscore; ans_x = tmp; fscore_valid = true;
	}
	// 6) -lt
	tmp = -lt;
	tmpscore = gamma * (tmp - dLdx(x, y)[0]) * (tmp - dLdx(x, y)[0])
	  + lambda_2 * (tmp - dIdx(x, y)[0]) * (tmp - dIdx(x, y)[0]) * smoothness_map(x, y)[0]
	  - lambda_1 * (k * lt);
	if (!fscore_valid || fscore > tmpscore) {
	  fscore = tmpscore; ans_x = tmp; fscore_valid = true;
	}
	Psi_x(x, y)[0] = ans_x;
      }
    }
    for (int y = 0; y < B_large.height; y++) {
      for (int x = 0; x < B_large.width; x++) {
	fscore_valid = false;
	// 1) Quadratic region:
	//  2 gamma (Psi_y - deriv_y L) + 2 lambda_2 (Psi_y - deriv_y I) .* mask
	//   + lambda_1 (-2a Psi_y) = 0.
	// Rearranging gives:
	//     (gamma + lambda_2 - a * lambda_1) psi_y = (gamma deriv_y L + lambda_2 deriv_y I .* mask)
	tmp = (gamma * dLdy(x, y)[0] + lambda_2 * dIdy(x, y)[0] * smoothness_map(x, y)[0])
	  / (gamma + lambda_2 - a * lambda_1);
	tmpscore = gamma * (tmp - dLdy(x, y)[0]) * (tmp - dLdy(x, y)[0])
	  + lambda_2 * (tmp - dIdy(x, y)[0]) * (tmp - dIdy(x, y)[0]) * smoothness_map(x, y)[0]
	  + lambda_1 * (-(a*tmp*tmp + b));
	if (fabs(tmp) > lt && (!fscore_valid || fscore > tmpscore)) {
	  fscore = tmpscore; ans_y = tmp; fscore_valid = true;
	}
	// 2) Positive linear region:
	//  2 gamma (Psi_y - deriv_y L) + 2 lambda_2 (Psi_y - deriv_y I) .* mask
	//   + lambda_1 (-k) = 0.
	// Rearranging gives:
	//     (gamma + lambda_2) psi_y = (gamma deriv_y L + lambda_2 deriv_y I .* mask + lambda_1 k)
	tmp = (gamma * dLdy(x, y)[0] + lambda_2 * dIdy(x, y)[0] * smoothness_map(x, y)[0] + lambda_1 * k)
	  / (gamma + lambda_2);
	tmpscore = gamma * (tmp - dLdy(x, y)[0]) * (tmp - dLdy(x, y)[0])
	  + lambda_2 * (tmp - dIdy(x, y)[0]) * (tmp - dIdy(x, y)[0]) * smoothness_map(x, y)[0]
	  + lambda_1 * (-k * tmp);
	if (tmp >= 0 && tmp <= lt && (!fscore_valid || fscore > tmpscore)) {
	  fscore = tmpscore; ans_y = tmp; fscore_valid = true;
	}
	// 3) Negative linear region:
	//  2 gamma (Psi_y - deriv_y L) + 2 lambda_2 (Psi_y - deriv_y I) .* mask
	//   + lambda_1 (k) = 0.
	// Rearranging gives:
	//     (gamma + lambda_2) psi_y = (gamma deriv_y L + lambda_2 deriv_y I .* mask - lambda_1 k)
	tmp = (gamma * dLdy(x, y)[0] + lambda_2 * dIdy(x, y)[0] * smoothness_map(x, y)[0] - lambda_1 * k)
	  / (gamma + lambda_2);
	tmpscore = gamma * (tmp - dLdy(x, y)[0]) * (tmp - dLdy(x, y)[0])
	  + lambda_2 * (tmp - dIdy(x, y)[0]) * (tmp - dIdy(x, y)[0]) * smoothness_map(x, y)[0]
	  + lambda_1 * (k * tmp);
	if (tmp >= 0 && tmp <= lt && (!fscore_valid || fscore > tmpscore)) {
	  fscore = tmpscore; ans_y = tmp; fscore_valid = true;
	} 
	// 4) Zero
	tmp = 0.f;
	tmpscore = gamma * (tmp - dLdy(x, y)[0]) * (tmp - dLdy(x, y)[0])
	  + lambda_2 * (tmp - dIdy(x, y)[0]) * (tmp - dIdy(x, y)[0]) * smoothness_map(x, y)[0];
	if (!fscore_valid || fscore > tmpscore) {
	  fscore = tmpscore; ans_y = tmp; fscore_valid = true;
	}
	// 5) lt
	tmp = lt;
	tmpscore = gamma * (tmp - dLdy(x, y)[0]) * (tmp - dLdy(x, y)[0])
	  + lambda_2 * (tmp - dIdy(x, y)[0]) * (tmp - dIdy(x, y)[0]) * smoothness_map(x, y)[0]
	  - lambda_1 * (-k * lt);
	if (!fscore_valid || fscore > tmpscore) {
	  fscore = tmpscore; ans_y = tmp; fscore_valid = true;
	}
	// 6) -lt
	tmp = -lt;
	tmpscore = gamma * (tmp - dLdy(x, y)[0]) * (tmp - dLdy(x, y)[0])
	  + lambda_2 * (tmp - dIdy(x, y)[0]) * (tmp - dIdy(x, y)[0]) * smoothness_map(x, y)[0]
	  - lambda_1 * (k * lt);
	if (!fscore_valid || fscore > tmpscore) {
	  fscore = tmpscore; ans_y = tmp; fscore_valid = true;
	}
	Psi_y(x, y)[0] = ans_y;
      }
    }    

    FPsi_x = Psi_x.copy(); FourierTransform(FPsi_x);
    FPsi_y = Psi_y.copy(); FourierTransform(FPsi_y);
    /******************************************/
    /* Optimize over L                        */
    /******************************************/
    // Fix Psi. Then the gradient of the objective becomes, in the Fourier domain:
    //    sum w_i F(K)^T F(deriv_i)^T (F(K) F(deriv_i) F(L) - F(deriv_i) F(I))
    //           + gamma F(deriv_x)^T (F(deriv_x) F(L) - F(Psi_x)) + ... = 0
    // Solving this yields F(L) = N/D,  where
    //   N = sum w_i F(K)^T |F(deriv_i)|^2 F(I) + gamma (F(deriv_x)^T F(Psi_x) +  ... )
    //   D = sum w_i |F(K)|^2 |F(deriv_i)|^2  + gamma |F(deriv_x)|^2 + |F(deriv_y)|^2
    // Note that the first term of N and D are independent of L or Psi or gamma.
    Image denominator = denominator_base.copy();
    Image numerator = numerator_base.copy();
    // Append the variable terms.
    for (int y = 0; y < B_large.height; y++)
      for (int x = 0; x < B_large.width; x++) {
	denominator(x, y)[0] += gamma * (FDeriv[1](x, y)[0] * FDeriv[1](x, y)[0]
					 + FDeriv[1](x, y)[1] * FDeriv[1](x, y)[1]);
	denominator(x, y)[0] += gamma * (FDeriv[3](x, y)[0] * FDeriv[3](x, y)[0]
					 + FDeriv[3](x, y)[1] * FDeriv[3](x, y)[1]);
	numerator(x, y)[0] += gamma * (FDeriv[1](x, y)[0] * FPsi_x(x, y)[0]
				       - FDeriv[1](x, y)[1] * FPsi_x(x, y)[1]);
	numerator(x, y)[0] += gamma * (FDeriv[3](x, y)[0] * FPsi_y(x, y)[0]
				       - FDeriv[3](x, y)[1] * FPsi_y(x, y)[1]);
      }
    ComplexDivide::apply(numerator, denominator, false);
    InverseFourierTransform(numerator);
    L = ComplexReal::apply(numerator);

    char filename_c[20];
    sprintf(filename_c, "output%02d.tmp", iterations);
    std::string filename(filename_c);
    FileTMP::save(L, filename, "float");

    /******************************************/
    /* Bookkeeping                            */
    /******************************************/
    lambda_1 /= 1.2f; lambda_2 /= 1.5f; gamma *= 2.f;
  }
  // Crop L as before.
  L = Crop::apply(L, x_padding, y_padding, B.width, B.height);
  return L;
#endif
}

/*
 * A poor man's version of "Reducing Boundary Artifacts in Image Deconvolution" (ICCP 2008)
 * by Liu and Jia.
 */
Image Deconvolution::applyPadding(Window B) {
  // Calculate the margin size.
  int alpha = 1; 
  if (B.width / 3 < alpha) alpha = B.width / 3;
  if (B.height / 3 < alpha) alpha = B.height / 3;
  int x_padding = B.width / 2;
  int y_padding = B.height / 2;
  if (x_padding < alpha * 3) x_padding = alpha * 3;
  if (y_padding < alpha * 3) y_padding = alpha * 3;

  // Prepare the enlarged canvas.
  float* prev = new float[B.channels];
  Image ret = Crop::apply(B, -x_padding, -y_padding, 0,
			  B.width+x_padding*2, B.height+y_padding*2, B.frames);
  for (int t = 0; t < B.frames; t++) {
    // Populate the top 'A' region.
    for (int y = 0; y < alpha; y++) {
      int realy = y; // alpha - 1 - y;
      memcpy(ret(x_padding, realy, t), ret(x_padding, y - alpha + B.height + y_padding, t),
	     sizeof(float) * B.channels * B.width);
      memcpy(ret(x_padding, y_padding - alpha + realy, t), ret(x_padding, y + y_padding, t),
	     sizeof(float) * B.channels * B.width);
    }
    for (int y = alpha; y < y_padding - alpha; y++) {
      // interpolate towards the bottom boundary.
      float weight = 1.f / (y_padding - alpha - (y-1));
      for (int x = x_padding; x < x_padding + B.width; x++)
	for (int c = 0; c < B.channels; c++)
	  ret(x, y, t)[c] = ret(x, y-1, t)[c] * (1.f - weight) + ret(x, y_padding - alpha, t)[c] * weight;
      // Blur with neighbors, more increasingly at the center.
      for (int c = 0; c < B.channels; c++)
	prev[c] = ret(x_padding, y, t)[c];
      float wing = 0.1f + 0.2f * (1.f - fabs(y_padding * 0.5f - y) / (y_padding * 0.5f));
      float center = 1.f - wing * 2.f;
      for (int x = x_padding; x < x_padding + B.width - 1; x++) {
	for (int c = 0; c < B.channels; c++) {
	  float tmp = ret(x, y, t)[c];
	  ret(x, y, t)[c] = prev[c] * wing + ret(x+1, y, t)[c] * wing + tmp * center;
	  prev[c] = tmp;
	}
      }
    }
    // Populate the bottom 'A' region
    for (int y = 0; y < y_padding; y++) {
      memcpy(ret(x_padding, y + B.height + y_padding, t), ret(x_padding, y, t),
	     sizeof(float) * B.channels * B.width);
    // Populate the left 'C-B-C' region
    for (int y = 0; y < B.height + y_padding * 2; y++) {
      for (int x = 0; x < alpha; x++) {
	int realx = x; // alpha - 1 - x;
	for (int c = 0; c < B.channels; c++) {
	  ret(realx, y, t)[c] = ret(B.width + x_padding - alpha + x, y, t)[c];
	  ret(x_padding - alpha + realx, y, t)[c] = ret(x_padding + x, y, t)[c];
	}
      }
    }
    for (int x = alpha; x < x_padding - alpha; x++) {
      // interpolate towards the right boundary.
      float weight = 1.f / (x_padding - alpha - (x-1));
      for (int y = 0; y < B.height + y_padding * 2; y++) {
	for (int c = 0; c < B.channels; c++) {
	  ret(x, y, t)[c] = ret(x-1, y, t)[c] * (1.f - weight) + ret(x_padding - alpha, y, t)[c] * weight;
	}
      }
      // Blur with neighbors, more increasingly at the center.
      for (int c = 0; c < B.channels; c++)
	prev[c] = ret(x, 0, t)[c];
      float wing = 0.1f + 0.2f * (1.f - fabs(x_padding * 0.5f - x) / (x_padding * 0.5f));
      float center = 1.f - wing * 2.f;
      for (int y = 0; y < B.height + y_padding * 2 - 1; y++) {
	for (int c = 0; c < B.channels; c++) {
	  float tmp = ret(x, y, t)[c];
	  ret(x, y, t)[c] = prev[c] * wing + ret(x, y+1, t)[c] * wing + tmp * center;
	  prev[c] = tmp;
	}
      }
    }
    // Populate the right 'C-B-C' region
    for (int y = 0; y < B.height + y_padding * 2; y++)
      memcpy(ret(B.width + x_padding, y, t), ret(0, y, t), sizeof(float)
	     * B.channels * x_padding);
    }
  }
  ret = Crop::apply(ret, x_padding/2, y_padding/2, 0, B.width + x_padding, B.height + y_padding, B.frames);
  delete[] prev;
  return ret;
}
  
  Image Deconvolution::applyCho2009(Window Blurred, Window K) {
#ifdef NO_FFTW
  panic("FFTW library has not been linked. Please recompile with proper flags.\n");
  return Image(1,1,1,1);
#else
  assert(K.width % 2 == 1 && K.height % 2 ==1,
	 "The kernel dimensions must be odd.\n");
  assert(K.channels == 1 && K.frames == 1 && Blurred.frames == 1,
	 "The kernel must be single-channel, and both the kernel and blurred\n"
	 "image must be single-framed.\n");

  // omega_* = 50 / (2^q) where q is the order of the derivative, alpha = 0.1
  // want to minimize w.r.t. L:

  // sum w_i | K * (deriv_i L) - (deriv B) | ^ 2 + alpha | grad L | ^ 2
  // In Fourier domain,
  //  sum w_i |F(K) F(deriv_i L) - F(deriv_i B)|^2 + alpha |F(grad L)|^2
  // sum w_i |F(K).* F(deriv_i).*F(L) - F(deriv_i).*F(B)|^2 + alpha ( |F(dx).*F(L)|^2 + |F(dy).*F(L)|^2)
  // Differentiating w.r.t. each element of F(L) and setting to zero, we get
  // F(L) = F(K)^T F(B) sum_i w_i |F(deriv_i)|^2  divided by
  //          |F(K)|^2 sum_i w_i |F(deriv_i)|^2  + alpha (|F(dx)|^2+|F(dy)|^2)

  Image B  = applyPadding(Blurred);
  FileTMP::save(B, std::string("padded.tmp"), "float");

  float alpha = 1.f; // TODO
  Image FK = KernelEstimation::EnlargeKernel(K, B.width, B.height);
  Image FB = RealComplex::apply(Transpose::apply(B, 'c', 't'));
  FFT::apply(FK, true, true, false);
  FFT::apply(FB, true, true, false);
  Image FK2 = FK.copy();
  ComplexMultiply::apply(FK2, FK, true);
  Image SumDeriv(B.width, B.height, 1, 2);
  Image SumGrad(B.width, B.height, 1, 2);
  for (int i = 0; i <= 5; i++) {
    float w_i;
    Image FDeriv(B.width, B.height, 1, 2);
    switch (i) {
    case 0: // Original
      w_i = 50.f;
      FDeriv(0, 0, 0)[0] = 1.f; break;
    case 1: // dx
      w_i = 25.f;
      FDeriv(0, 0, 0)[0] = -1.f;
      FDeriv(1, 0, 0)[0] = 1.f; break;
    case 2: // dxx
      w_i = 12.5f;
      FDeriv(0, 0, 0)[0] = 1.f;
      FDeriv(1, 0, 0)[0] = -2.f;
      FDeriv(2, 0, 0)[0] = 1.f; break;
    case 3: // dy
      w_i = 25.f;
      FDeriv(0, 0, 0)[0] = -1.f;
      FDeriv(0, 1, 0)[0] = 1.f; break;
    case 4: // dyy
      w_i = 12.5f;
      FDeriv(0, 0, 0)[0] = 1.f;
      FDeriv(0, 1, 0)[0] = -2.f;
      FDeriv(0, 2, 0)[0] = 1.f; break;
    case 5: // dxy
      w_i = 12.5f;
      FDeriv(0, 0, 0)[0] = 1.f;
      FDeriv(1, 0, 0)[0] = -1;
      FDeriv(0, 1, 0)[0] = -1;
      FDeriv(1, 1, 0)[0] = 1; break;
    }
    FFT::apply(FDeriv, true, true, false);
    Image FDeriv2 = FDeriv.copy();
    ComplexMultiply::apply(FDeriv2, FDeriv, true);
    if (i == 1 || i == 3)
      Add::apply(SumGrad, FDeriv2);
    Scale::apply(FDeriv2, w_i);
    Add::apply(SumDeriv, FDeriv2);
  }
  Scale::apply(SumGrad, alpha);
  // Recall the following:
  // F(L) = F(K)^T F(B) sum_i w_i |F(deriv_i)|^2  divided by
  //          |F(K)|^2 sum_i w_i |F(deriv_i)|^2  + alpha (|F(dx)|^2+|F(dy)|^2)
  // In our diction, we have
  // FK^T FB SumDeriv / (FK2 SumDeriv + SumGrad)
  ComplexConjugate::apply(FK);
  ComplexMultiply::apply(FK, SumDeriv, false);
  ComplexMultiply::apply(FK2, SumDeriv, false);
  Add::apply(FK2, SumGrad);
  ComplexDivide::apply(FK, FK2, false); // FK contains the running result.
  for (int t = 0; t < B.channels; t++) {
    for (int y = 0; y < B.height; y++) {
      for (int x = 0; x < B.width; x++) {
	float tmp = FB(x, y, t)[0] * FK(x, y)[0] - FB(x, y, t)[1] * FK(x, y)[1];
	FB(x, y, t)[1] = FB(x, y, t)[0] * FK(x, y)[1] + FB(x, y, t)[1] * FK(x, y)[0];
	FB(x, y, t)[0] = tmp;
      }
    }
  }
  IFFT::apply(FB, true, true, false);
  const int x_padding = (B.width - Blurred.width) / 2;
  const int y_padding = (B.height - Blurred.height) / 2;
  return Crop::apply(Transpose::apply(ComplexReal::apply(FB), 'c', 't'),
		     x_padding, y_padding, 0, Blurred.width, Blurred.height, Blurred.frames);  
#endif
}

#include "footer.h"

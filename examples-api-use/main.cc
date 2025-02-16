// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
//
// This code is public domain
// (but note, once linked against the led-matrix library, this is
// covered by the GPL v2)
//
// This is a grab-bag of various demos and not very readable.
#include "led-matrix.h"

#include "pixel-mapper.h"
#include "graphics.h"

#include <iostream>
#include <assert.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <networktables/NetworkTableInstance.h>
#include <algorithm>

using namespace std;

#define TERM_ERR  "\033[1;31m"
#define TERM_NORM "\033[0m"

using namespace rgb_matrix;

rgb_matrix::Font font;
Canvas *canvas;

nt::NetworkTableInstance ntinst;

volatile bool interrupt_received = false;
volatile bool show_text = false;
static void InterruptHandler(int signo) {
  interrupt_received = true;
}


class ImageScroller {
public:
  ~ImageScroller () {}

  // Scroll image with "scroll_jumps" pixels every "scroll_ms" milliseconds.
  // If "scroll_ms" is negative, don't do any scrolling.
  ImageScroller(RGBMatrix *m, int scroll_jumps, int scroll_ms = 30)
    : canvas_(m), scroll_jumps_(scroll_jumps),
      scroll_ms_(scroll_ms),
      horizontal_position_(0),
      matrix_(m) {
    offscreen_ = matrix_->CreateFrameCanvas();
  }

  // _very_ simplified. Can only read binary P6 PPM. Expects newlines in headers
  // Not really robust. Use at your own risk :)
  // This allows reload of an image while things are running, e.g. you can
  // live-update the content.
  bool LoadPPM(const char *filename) {
    FILE *f = fopen(filename, "r");
    // check if file exists
    if (f == NULL && access(filename, F_OK) == -1) {
      fprintf(stderr, "File \"%s\" doesn't exist\n", filename);
      return false;
    }
    if (f == NULL) return false;
    char header_buf[256];
    const char *line = ReadLine(f, header_buf, sizeof(header_buf));
#define EXIT_WITH_MSG(m) { fprintf(stderr, "%s: %s |%s", filename, m, line); \
      fclose(f); return false; }
    if (sscanf(line, "P6 ") == EOF)
      EXIT_WITH_MSG("Can only handle P6 as PPM type.");
    line = ReadLine(f, header_buf, sizeof(header_buf));
    int new_width, new_height;
    if (!line || sscanf(line, "%d %d ", &new_width, &new_height) != 2)
      EXIT_WITH_MSG("Width/height expected");
    int value;
    line = ReadLine(f, header_buf, sizeof(header_buf));
    if (!line || sscanf(line, "%d ", &value) != 1 || value != 255)
      EXIT_WITH_MSG("Only 255 for maxval allowed.");
    const size_t pixel_count = new_width * new_height;
    Pixel *new_image = new Pixel [ pixel_count ];
    assert(sizeof(Pixel) == 3);   // we make that assumption.
    if (fread(new_image, sizeof(Pixel), pixel_count, f) != pixel_count) {
      line = "";
      EXIT_WITH_MSG("Not enough pixels read.");
    }
#undef EXIT_WITH_MSG
    fclose(f);
    fprintf(stderr, "Read image '%s' with %dx%d\n", filename,
            new_width, new_height);
    horizontal_position_ = 0;
    MutexLock l(&mutex_new_image_);
    new_image_.Delete();  // in case we reload faster than is picked up
    new_image_.image = new_image;
    new_image_.width = new_width;
    new_image_.height = new_height;
    return true;
  }

  void Run() {
    const int screen_height = offscreen_->height();
    const int screen_width = offscreen_->width();
    int ctr = 0;
    long long start_time = time(NULL);
    while (!interrupt_received) {
      //std::cout << ctr << "\n";

      //check if two seconds passed
      long long cur_time = time(NULL);
      if (cur_time - start_time > 3) {
        show_text = true;
        break;
      }

      {
        MutexLock l(&mutex_new_image_);
        if (new_image_.IsValid()) {
          current_image_.Delete();
          current_image_ = new_image_;
          new_image_.Reset();
        }
      }
      if (!current_image_.IsValid()) {
        usleep(100 * 1000);
        continue;
      }
      for (int x = 0; x < screen_width; ++x) {
        for (int y = 0; y < screen_height; ++y) {
          const Pixel &p = current_image_.getPixel(
            (horizontal_position_ + x) % current_image_.width, y);
          offscreen_->SetPixel(x, y, p.red, p.green, p.blue);
        }
      }
      offscreen_ = matrix_->SwapOnVSync(offscreen_);
      horizontal_position_ += scroll_jumps_;
      if (horizontal_position_ < 0) horizontal_position_ = current_image_.width;
      if (scroll_ms_ <= 0) {
        // No scrolling. We don't need the image anymore.
        current_image_.Delete();
      } else {
        usleep(scroll_ms_ * 1000);
      }
      ctr++;
    }
  }

private:
  inline Canvas *canvas() { return canvas_; }

  struct Pixel {
    Pixel() : red(0), green(0), blue(0){}
    uint8_t red;
    uint8_t green;
    uint8_t blue;
  };

  struct Image {
    Image() : width(-1), height(-1), image(NULL) {}
    ~Image() { Delete(); }
    void Delete() { delete [] image; Reset(); }
    void Reset() { image = NULL; width = -1; height = -1; }
    inline bool IsValid() { return image && height > 0 && width > 0; }
    const Pixel &getPixel(int x, int y) {
      static Pixel black;
      if (x < 0 || x >= width || y < 0 || y >= height) return black;
      return image[x + width * y];
    }

    int width;
    int height;
    Pixel *image;
  };

  // Read line, skip comments.
  char *ReadLine(FILE *f, char *buffer, size_t len) {
    char *result;
    do {
      result = fgets(buffer, len, f);
    } while (result != NULL && result[0] == '#');
    return result;
  }

  Canvas *const canvas_;

  const int scroll_jumps_;
  const int scroll_ms_;

  // Current image is only manipulated in our thread.
  Image current_image_;

  // New image can be loaded from another thread, then taken over in main thread
  Mutex mutex_new_image_;
  Image new_image_;

  int32_t horizontal_position_;

  RGBMatrix* matrix_;
  FrameCanvas* offscreen_;
};



static int usage(const char *progname) {
  fprintf(stderr, "usage: %s <options> -D <demo-nr> [optional parameter]\n",
          progname);
  fprintf(stderr, "Options:\n");
  fprintf(stderr,
          "\t-D <demo-nr>              : Always needs to be set\n"
          );


  rgb_matrix::PrintMatrixFlags(stderr);

  fprintf(stderr, "Demos, choosen with -D\n");
  fprintf(stderr, "\t0  - some rotating square\n"
          "\t1  - forward scrolling an image (-m <scroll-ms>)\n"
          "\t2  - backward scrolling an image (-m <scroll-ms>)\n"
          "\t3  - test image: a square\n"
          "\t4  - Pulsing color\n"
          "\t5  - Grayscale Block\n"
          "\t6  - Abelian sandpile model (-m <time-step-ms>)\n"
          "\t7  - Conway's game of life (-m <time-step-ms>)\n"
          "\t8  - Langton's ant (-m <time-step-ms>)\n"
          "\t9  - Volume bars (-m <time-step-ms>)\n"
          "\t10 - Evolution of color (-m <time-step-ms>)\n"
          "\t11 - Brightness pulse generator\n");
  fprintf(stderr, "Example:\n\t%s -D 1 runtext.ppm\n"
          "Scrolls the runtext until Ctrl-C is pressed\n", progname);
  return 1;
}

void SHOW_TEXT (char* text) {
  Color color(85, 173, 17);
  Color bg_color(0, 0, 0);
  Color flood_color(0, 0, 0);
  Color outline_color(255, 255, 255);

  long long start_time = time(NULL);

  while (!interrupt_received) {
    long long cur_time = time(NULL);
    if (cur_time - start_time > 3) {
      break;
    }

    //std::cout<<"1\n";
    int x = 8;
    int y = 8;
    string line = text;
    const size_t last = line.length();

    //std::cout<<"2\n";

    //std::cout<<"2a\n";
    bool line_empty = strlen(line) == 0;
    //std::cout<<"2b\n";
    if ((y + font.height() > canvas->height()) || line_empty) {
      canvas->Fill(flood_color.r, flood_color.g, flood_color.b);
      y = 0;
    }
    //std::cout<<"2c\n";
    if (line_empty)
      return;
    
    //std::cout<<"3\n";
    
    // The regular text. Unless we already have filled the background with
    // the outline font, we also fill the background here.
    rgb_matrix::DrawText(canvas, font, x, y + font.baseline(),
                          color, &bg_color, line,
                          0 /*letter spacing*/);
    y += font.height();
  }
}

int main(int argc, char *argv[]) {
  ntinst = nt::NetworkTableInstance::GetDefault();
  //ntinst.StartServer();
  //ntinst.StartClientTeam(3749);
  ntinst.StartClient("localhost");

  int scroll_ms = 20;

  const char *demo_parameter = NULL;
  RGBMatrix::Options matrix_options;
  rgb_matrix::RuntimeOptions runtime_opt;

  // These are the defaults when no command-line flags are given.
  matrix_options.rows = 32;
  matrix_options.cols = 64;
  matrix_options.hardware_mapping = "adafruit-hat";
  matrix_options.chain_length = 1;
  matrix_options.parallel = 1;

  // First things first: extract the command line flags that contain
  // relevant matrix options.
  if (!ParseOptionsFromFlags(&argc, &argv, &matrix_options, &runtime_opt)) {
    return usage(argv[0]);
  }

  int opt;
  while ((opt = getopt(argc, argv, "r:P:c:p:b:m:LR:")) != -1) {
    switch (opt) {
      case 'm':
        scroll_ms = atoi(optarg);
        break;

      default: /* '?' */
        return usage(argv[0]);
    }
  }

  if (optind < argc) {
    demo_parameter = argv[optind];
  }

  RGBMatrix *matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
  if (matrix == NULL)
    return 1;

  printf("Size: %dx%d. Hardware gpio mapping: %s\n",
         matrix->width(), matrix->height(), matrix_options.hardware_mapping);

  canvas = matrix;

  if (!demo_parameter) return -1;

  font.LoadFont("../fonts/16-13O.bdf");

  ImageScroller *scroller = new ImageScroller(matrix, 1, scroll_ms);

  std::string ppmFileStr = std::string("../images/")+std::string(demo_parameter);

  const char* ppmFile = ppmFileStr.c_str();

  if (!scroller->LoadPPM(ppmFile))
    return 1;

  // Set up an interrupt handler to be able to stop animations while they go
  // on. Each demo tests for while (!interrupt_received) {},
  // so they exit as soon as they get a signal.
  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);

  printf("Press <CTRL-C> to exit and reset LEDs\n");

  // Now, run our particular demo; it will exit when it sees interrupt_received.
  scroll_img:
  std::cout << "running scrolled\n";
  ntinst.GetTable("LED")->GetEntry("status").SetString("scrolled");

  scroller->Run();

  if (show_text) {
    canvas->Clear();

    ntinst.GetTable("LED")->GetEntry("status").SetString("text");

    std::cout << "showing text\n";

    show_text = false;

    SHOW_TEXT("VENOM");

    goto scroll_img;
  }

  delete scroller;
  delete canvas;

  printf("Received CTRL-C. Exiting.\n");
  return 0;
}

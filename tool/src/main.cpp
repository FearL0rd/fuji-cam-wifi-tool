#include <stdio.h>

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

#include "log.hpp"
#include "comm.hpp"
#include "commands.hpp"

#include "linenoise.h"

#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <atomic>
#include <algorithm>

#ifdef WITH_OPENCV
#include <opencv2/opencv.hpp>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <unistd.h>

using namespace cv;
#endif

namespace fcwt {

sock sockfd;

log_settings log_conf;

#ifdef WITH_OPENCV
#define WIN_NAME "Display Window"

// On X-T100 at least the auto-focus points are specified with these ranges.
// Not sure how we get the ranges from the camera..
//
// The area around misses a selection of each of those points
//
// Needs to be in AF-S mode not manual
#define POINTS_X 0xd
#define POINTS_Y 0x7

Rect cur_focus(0,0,0,0);

static void onMouse( int event, int x, int y, int, void* )
{
    if( event != EVENT_LBUTTONDOWN )
        return;

    // TODO: Refuse if not AF-S

    Rect win_size = getWindowImageRect(WIN_NAME);
    float x_perc = (float)x / win_size.width;
    float y_perc = (float)y / win_size.height;

    auto_focus_point point = 0;
    point.x = x_perc * (2+POINTS_X);
    point.y = y_perc * (2+POINTS_Y);

    if( point.x < 1 || point.x > POINTS_X || point.y < 1 || point.y > POINTS_Y )
        return;

    cur_focus.x = (float)point.x / (2+POINTS_X) * win_size.width;
    cur_focus.y = (float)point.y / (2+POINTS_Y) * win_size.height;
    cur_focus.width = win_size.width / (2+POINTS_X);
    cur_focus.height = win_size.height / (2+POINTS_Y);

    log(LOG_DEBUG, string_format("Set focus point %d x %d (%f%% x %f%%)", point.x, point.y, x_perc, y_perc));

    if (update_setting(sockfd, point)) {
        // TODO: Decode if it got focused or not successfully (red/green bracket)
        current_properties settings;
        if (current_settings(sockfd, settings))
          print(settings);
    } else {
        log(LOG_ERROR, string_format("Failed to adjust focus point"));
    }
}

//#define CV_TEST

int setup_v4l2(std::string v4l2lo_dev, Mat image) {
    int v4l2lo = open(v4l2lo_dev.c_str(), O_WRONLY);
    if(v4l2lo < 0) {
        log(LOG_ERROR, string_format("Error opening v4l2l device: %s", strerror(errno)));
        return v4l2lo;
    }

    struct v4l2_format v;
    int t;
    v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    t = ioctl(v4l2lo, VIDIOC_G_FMT, &v);
    if( t < 0 ) {
        log(LOG_ERROR, string_format("ioctl error with setting up v4l2l device: %d", t));
        close(v4l2lo);
        return -1;
    }

    v.fmt.pix.width = image.cols;
    v.fmt.pix.height = image.rows;
    v.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
    v.fmt.pix.sizeimage = image.total() * image.elemSize();
    t = ioctl(v4l2lo, VIDIOC_S_FMT, &v);
    if( t < 0 ) {
        log(LOG_ERROR, string_format("ioctl error with v4l2l device format: %d", t));
        close(v4l2lo);
        return -1;
    }

    return v4l2lo;
}

void image_stream_cv_main(std::atomic<bool>& flag, std::string v4l2lo_dev = "") {
  log(LOG_INFO, "image_stream_cv_main");
#ifndef CV_TEST
  sock const sockfd3 = connect_to_camera(jpg_stream_server_port);

  std::vector<uint8_t> buffer(1024 * 1024);

  if (sockfd3 <= 0) return;
#endif

  int v4l2lo = 0;

  namedWindow( WIN_NAME, WINDOW_AUTOSIZE );// Create a window for display.
  setMouseCallback(WIN_NAME, onMouse);
  while (flag) {
    if( getWindowProperty(WIN_NAME, WND_PROP_AUTOSIZE) == -1 )
        break;

#ifdef CV_TEST
    Mat decodedImage = Mat::zeros( 480, 640, CV_8UC3 );
#else
    size_t receivedBytes =
        fuji_receive(sockfd3, buffer.data(), buffer.size());


    int const header = 14;  // not sure what's in the first 14 bytes
    Mat rawData = Mat( 1, receivedBytes, CV_8UC1, &buffer[header]);
    Mat decodedImage  =  imdecode( rawData , cv::IMREAD_COLOR );
#endif

    if ( decodedImage.data == NULL )
    {
        log(LOG_WARN, "couldn't decode image");
    }
    Mat displayImage = decodedImage.clone();
    if( cur_focus.height > 0 )
        rectangle(displayImage, cur_focus, Scalar(255, 255, 255));
    imshow( WIN_NAME, displayImage );

    // Maybe copy to v4l2lo device
    if( v4l2lo_dev.length() > 0 && v4l2lo == 0 )
        v4l2lo = setup_v4l2(v4l2lo_dev, decodedImage);

    if(v4l2lo > 0) {
        size_t written = write(v4l2lo, decodedImage.data, decodedImage.total() * decodedImage.elemSize());
        if( written < 0 ) {
            log(LOG_ERROR, string_format("error writing data to v4l2l: %ld", written));
            close(v4l2lo);
            v4l2lo = -1;
        }
    }

    waitKey(1);
  }

  destroyAllWindows();
}
#endif

void image_stream_main(std::atomic<bool>& flag) {
  log(LOG_INFO, "image_stream_main");
  sock const sockfd3 = connect_to_camera(jpg_stream_server_port);

  std::vector<uint8_t> buffer(1024 * 1024);

  if (sockfd3 <= 0) return;

  unsigned int image = 0;
  while (flag) {
    size_t receivedBytes =
        fuji_receive(sockfd3, buffer.data(), buffer.size());
    log(LOG_DEBUG, string_format("image_stream_main received %zd bytes", receivedBytes));

    char filename[1024];
    snprintf(filename, sizeof(filename), "out/img_%d.jpg", image++);
    FILE* file = fopen(filename, "wb");
    if (file) {
      // First 14 bytes like:
      // uint32_t 0
      // uint32_t frame_no (increments one each time a frame is sent)
      // rest are 0s
      int const header = 14;  // not sure what's in the first 14 bytes
      fwrite(&buffer[header], receivedBytes, 1, file);
      fclose(file);
    } else {
      log(LOG_WARN, string_format("image_stream_main Failed to create file %s", filename));
    }
  }
}

char const* commandStrings[] = {"connect", "shutter", "stream",
                                "info", "set_iso", "set_aperture", "aperture",
                                "shutter_speed", "set_shutter_speed",
                                "white_balance", "current_settings",
                                "film_simulation", "timer", "flash",
                                "exposure_compensation", "set_exposure_compensation",
                                "focus_point", "unlock_focus",
                                "start_record", "stop_record",
#ifdef WITH_OPENCV
                                "stream_cv",
#endif
};

enum class command {
  connect,
  shutter,
  stream,
  info,
  set_iso,
  set_aperture,
  aperture,
  shutter_speed,
  set_shutter_speed,
  white_balance,
  current_settings,
  film_simulation,
  timer,
  flash,
  exposure_compensation,
  set_exposure_compensation,
  focus_point,
  unlock_focus,
  start_record,
  stop_record,
#ifdef WITH_OPENCV
  stream_cv,
#endif
  unknown,
  count = unknown
};

static void completion(char const* buf, linenoiseCompletions* lc) {
  for (int i = 0; i < static_cast<int>(command::count); ++i) {
    char const* const cmd = commandStrings[i];
    if (strstr(cmd, buf) == cmd) linenoiseAddCompletion(lc, cmd);
  }
}

bool getline(std::string& line) {
  char* const str = linenoise("fcwt> ");
  if (!str) return false;

  line.assign(str);
  free(str);
  return true;
}

command parse_command(std::string const& line) {
  for (int i = 0; i < static_cast<int>(command::count); ++i) {
    if (line == commandStrings[i]) return static_cast<command>(i);
  }

  return command::unknown;
}

std::vector<std::string> split(std::string const& str,
                               int delimiter(int) = ::isspace) {
  std::vector<std::string> result;
  auto const itEnd = str.end();
  auto it = str.begin();
  while (it != itEnd) {
    it = std::find_if_not(it, itEnd, delimiter);
    if (it == itEnd) break;

    auto const it2 = std::find_if(it, itEnd, delimiter);
    result.emplace_back(it, it2);
    it = it2;
  }
  return result;
}

int main(int const argc, char const* argv[]) {
  uint8_t log_level = LOG_DEBUG;
  uint32_t cur_record_id = 0;

  if (argc > 1) {
    std::string arg = argv[1];
    if (arg == "-l" || arg == "--log-level")
      log_level = std::stoi(argv[2], 0, 0);
  }

  log_conf.level = log_level;

  /* Set the completion callback. This will be called every time the
   * user uses the <tab> key. */
  linenoiseSetCompletionCallback(completion);

  sock sockfd2;
  std::atomic<bool> imageStreamFlag(true);
  std::thread imageStreamThread;
#ifdef WITH_OPENCV
  std::thread imageStreamCVThread;
#endif
  std::vector<capability> caps;
  current_properties settings;

  std::string line;
  while (getline(line)) {
    linenoiseHistoryAdd(line.c_str());
    auto const splitLine = split(line);
    if (splitLine.empty()) continue;

    command cmd = parse_command(splitLine[0]);
    switch (cmd) {
      case command::connect: {
        if (sockfd <= 0) {
          sockfd = connect_to_camera(control_server_port);
          if (!init_control_connection(sockfd, "HackedClient", &caps))
            log(LOG_ERROR, "failure\n");
          else {
            log(LOG_INFO, "Received camera capabilities");
            print(caps);
            if (current_settings(sockfd, settings)) {
              log(LOG_INFO, "Received camera settings");
              print(settings);
            }
            sockfd2 = connect_to_camera(async_response_server_port);
          }
        } else {
          log(LOG_INFO, "already connected\n");
        }
      } break;
      case command::shutter: {
        if (!shutter(sockfd, sockfd2, "thumb.jpg")) log(LOG_ERROR, "failure\n");

      } break;

      case command::stream: {
        imageStreamThread =
            std::thread(([&]() { image_stream_main(imageStreamFlag); }));
      } break;

#ifdef WITH_OPENCV
      case command::stream_cv: {
        std::string v4l2lo_dev = "";
        if( splitLine.size() > 1 )
            v4l2lo_dev = splitLine[1];
        imageStreamCVThread =
            std::thread(([&]() { image_stream_cv_main(imageStreamFlag, v4l2lo_dev); }));
      } break;
#endif

      case command::info: {
        if (current_settings(sockfd, settings))
          print(settings);
      } break;

      case command::set_iso: {
        if (splitLine.size() > 1) {
          unsigned long iso = std::stoul(splitLine[1], 0, 0);
          log(LOG_DEBUG, string_format("%s(%lu)", splitLine[0].c_str(), iso));
          if (update_setting(sockfd, property_iso, iso)) {
            if (current_settings(sockfd, settings))
              print(settings);
          } else {
            log(LOG_ERROR, string_format("Failed to set ISO %lu", iso));
          }
        }
      } break;

      // this doesnt seem to work on x-t100
      case command::set_aperture: {
        if (splitLine.size() > 1) {
          uint32_t const aperture = static_cast<uint32_t>(std::stod(splitLine[1]) * 100.0);
          if (aperture > 0 && aperture < 6400 && 
              current_settings(sockfd, settings) && settings.values[property_aperture] > 0 &&
              aperture != settings.values[property_aperture]) {
            const fnumber_update_direction direction = aperture < settings.values[property_aperture] ? fnumber_decrement : fnumber_increment;
            uint32_t last_aperture = 0;
            do {
              last_aperture = settings.values[property_aperture];
              if (!update_setting(sockfd, direction))
                break;
            } while(current_settings(sockfd, settings) && 
                    settings.values[property_aperture] != last_aperture && 
                    aperture != settings.values[property_aperture] &&
                    direction == (aperture < settings.values[property_aperture] ? fnumber_decrement : fnumber_increment));
            print(settings);
          } 
        }
      } break;

      // parameter 1 / -1 to say in/out
      case command::aperture: {
        if (splitLine.size() > 1) {
          int aperture_stops = std::stoi(splitLine[1], 0, 0);
          log(LOG_DEBUG, string_format("%s(%i)", splitLine[0].c_str(), aperture_stops));
          if (aperture_stops != 0) {
            if (update_setting(sockfd, aperture_stops < 0 ? fnumber_decrement : fnumber_increment)) {
              if (current_settings(sockfd, settings))
                print(settings);
            } else {
              log(LOG_ERROR, string_format("Failed to adjust aperture %i", aperture_stops));
            }
          }
        }
      } break;

      case command::shutter_speed: {
        if (splitLine.size() > 1) {
          int shutter_stops = std::stoi(splitLine[1], 0, 0);
          log(LOG_DEBUG, string_format("%s(%i)", splitLine[0].c_str(), shutter_stops));
          if (shutter_stops != 0) {
            if (update_setting(sockfd, shutter_stops < 0 ? ss_decrement : ss_increment)) {
              if (current_settings(sockfd, settings))
                print(settings);
            } else {
              log(LOG_ERROR, string_format("Failed to adjust shutter speed %i", shutter_stops));
            }
          }
        }
      } break;

      case command::set_shutter_speed: {
        if (splitLine.size() > 1) {
          double nom, denom;
          int res = std::sscanf(splitLine[1].c_str(), "%lf/%lf", &nom, &denom);
          if (res > 0) {
            double new_speed = (res == 1 ? nom : nom / denom) * 1000000.0;
            if (current_settings(sockfd, settings) && settings.values[property_shutter_speed] > 0 &&
                new_speed != ss_to_microsec(settings.values[property_shutter_speed])) {
              const ss_update_direction direction = new_speed < ss_to_microsec(settings.values[property_shutter_speed]) ? ss_increment : ss_decrement;
              uint64_t last_speed = 0;
              do {
                last_speed = ss_to_microsec(settings.values[property_shutter_speed]);
                if (!update_setting(sockfd, direction))
                  break;
              } while(current_settings(sockfd, settings) &&
                      ss_to_microsec(settings.values[property_shutter_speed]) != last_speed &&
                      new_speed != ss_to_microsec(settings.values[property_shutter_speed]) &&
                      direction == (new_speed < ss_to_microsec(settings.values[property_shutter_speed]) ? ss_increment : ss_decrement));
              print(settings);
            }
          }
        }
      } break;

      case command::exposure_compensation: {
        if (splitLine.size() > 1) {
          int direction = std::stoi(splitLine[1], 0, 0);
          log(LOG_DEBUG, string_format("%s(%i)", splitLine[0].c_str(), direction));
          if (direction != 0) {
            if (update_setting(sockfd, direction < 0 ? exp_decrement : exp_increment)) {
              if (current_settings(sockfd, settings))
                print(settings);
            } else {
              log(LOG_ERROR, string_format("Failed to adjust exposure correction %i", direction));
            }
          }
        }
      } break;

      case command::set_exposure_compensation: {
        if (splitLine.size() > 1) {
          uint32_t const exp = static_cast<uint32_t>(std::stod(splitLine[1]) * 1000.0);
          if (current_settings(sockfd, settings) && exp != settings.values[property_exposure_compensation]) {
            const exp_update_direction direction = exp < settings.values[property_exposure_compensation] ? exp_decrement : exp_increment;
            uint32_t last_exp = 0;
            do {
              last_exp = settings.values[property_exposure_compensation];
              if (!update_setting(sockfd, direction))
                break;
            } while(current_settings(sockfd, settings) && 
                    settings.values[property_exposure_compensation] != last_exp && 
                    exp != settings.values[property_exposure_compensation] &&
                    direction == (exp < settings.values[property_exposure_compensation] ? exp_decrement : exp_increment));
            print(settings);
          } 
        }
      } break;

      case command::white_balance: {
        if (splitLine.size() > 1) {
          uint32_t const value = std::stoi(splitLine[1], 0, 0);
          log(LOG_DEBUG, string_format("%s(%d)", splitLine[0].c_str(), value));
          if (is_known_property_value(property_white_balance, value) && update_setting(sockfd, property_white_balance, value)) {
            if (current_settings(sockfd, settings))
              print(settings);
          } else {
            log(LOG_ERROR, string_format("Failed to set white_balance %d", value));
          }
        }
      } break;

      case command::film_simulation: {
        if (splitLine.size() > 1) {
          uint32_t const value = std::stoi(splitLine[1], 0, 0);
          log(LOG_DEBUG, string_format("%s (%d)", splitLine[0].c_str(), value));

          if (is_known_property_value(property_film_simulation, value) && update_setting(sockfd, property_film_simulation, value)) {
            if (current_settings(sockfd, settings))
              print(settings);
          } else {
            log(LOG_ERROR, string_format("Failed to set film simulation %d", value));
          }
        }
      } break;

      case command::flash: {
        if (splitLine.size() > 1) {
          uint32_t const value = std::stoi(splitLine[1], 0, 0);
          log(LOG_DEBUG, string_format("%s (%d)", splitLine[0].c_str(), value));

          if (is_known_property_value(property_flash, value) && update_setting(sockfd, property_flash, value)) {
            if (current_settings(sockfd, settings))
              print(settings);
          } else {
            log(LOG_ERROR, string_format("Failed to set flash mode  %d", value));
          }
        }
      } break;

      case command::timer: {
        if (splitLine.size() > 1) {
          uint32_t const value = std::stoi(splitLine[1], 0, 0);
          log(LOG_DEBUG, string_format("%s (%d)", splitLine[0].c_str(), value));

          if (is_known_property_value(property_self_timer, value) && update_setting(sockfd, property_self_timer, value)) {
            if (current_settings(sockfd, settings))
              print(settings);
          } else {
            log(LOG_ERROR, string_format("Failed to set timer %d", value));
          }
        }
      } break;

      case command::focus_point: {
        if (splitLine.size() == 3) {
          auto_focus_point point = 0;
          // needs to be set to af-s mode not manual
          point.x = std::stoi(splitLine[1], 0, 0);
          point.y = std::stoi(splitLine[2], 0, 0);
          if (point.x * point.y <= 0) {
            log(LOG_INFO, "Could not parse provided value");
            break;
          }

          if (update_setting(sockfd, point)) {
            if (current_settings(sockfd, settings))
              print(settings);
          } else {
            log(LOG_ERROR, string_format("Failed to adjust focus point"));
          }
        }
      } break;

      case command::unlock_focus: {
        if (splitLine.size() == 1) {
          if (unlock_focus(sockfd)) {
            if (current_settings(sockfd, settings))
              print(settings);
          } else {
            log(LOG_ERROR, string_format("Failed to unlock focus"));
          }
        }
      } break;

      case command::start_record: {
        if( cur_record_id ) {
            log(LOG_ERROR, string_format("Already recording, issue stop_record first"));
            break;
        }

        cur_record_id = start_record(sockfd);
        if (cur_record_id) {
            if (current_settings(sockfd, settings))
              print(settings);
        } else {
            log(LOG_ERROR, string_format("Failed to start recording"));
        }
      } break;

      case command::stop_record: {
        if( !cur_record_id ) {
            log(LOG_ERROR, string_format("Not recording, issue start_record first"));
            break;
        }

        if(stop_record(sockfd, cur_record_id)) {
          cur_record_id = 0;
          if (current_settings(sockfd, settings))
              print(settings);
        } else {
            log(LOG_ERROR, string_format("Failed to stop recording"));
        }
      } break;

      case command::current_settings: {
        if (current_settings(sockfd, settings))
          print(settings);
        else
          log(LOG_ERROR, "fail");
      } break;

      default: { log(LOG_ERROR, string_format("Unreconized command: %s", line.c_str())); }
    }
  }

  if (imageStreamThread.joinable()) {
    imageStreamFlag = false;
    imageStreamThread.join();
  }

#ifdef WITH_OPENCV
  if (imageStreamCVThread.joinable()) {
    imageStreamFlag = false;
    imageStreamCVThread.join();
  }
#endif

  terminate_control_connection(sockfd);

  return 0;
}

}  // namespace fcwt

int main(const int argc, char const* argv[]) { return fcwt::main(argc, argv); }

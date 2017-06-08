
#include <atomic>
#include <chrono>
#include <iostream>
#include <sys/time.h>

#include <backward.hpp>
#include <gflags/gflags.h>
#include <opencv/cv.h>
#include <opencv/highgui.h>
#include <pangolin/pangolin.h>

#include "DynSlam.h"
#include "Utils.h"

#include "../pfmLib/ImageIOpfm.h"
#include "PrecomputedDepthProvider.h"
#include "InstRecLib/VisoSparseSFProvider.h"

// Commandline arguments using gflags
DEFINE_string(dataset_root, "", "The root folder of the dataset to use.");

// Note: the [RIP] tags signal spots where I wasted more than 30 minutes debugging a small, silly
// issue.

// Handle SIGSEGV and its friends by printing sensible stack traces with code snippets.
// TODO(andrei): this is a hack, please remove or depend on backward directly.
backward::SignalHandling sh;

/// \brief Define these because OpenCV doesn't. Used in the `cv::flip` OpenCV function.
enum {
  kCvFlipVertical = 0,
  kCvFlipHorizontal = 1,
  kCvFlipBoth = -1
};

namespace dynslam {
namespace gui {

using namespace std;
using namespace instreclib::reconstruction;
using namespace instreclib::segmentation;

using namespace dynslam;
using namespace dynslam::utils;

static const int kUiWidth = 300;
static const float kPlotTimeIncrement = 0.1f;

/// TODO-LOW(andrei): Consider using QT or wxWidgets. Pangolin is limited in terms of the widgets it
/// supports. It doesn't even seem to support multiline text, or any reasonable way to display plain
/// labels or lists or anything... Might be better not to worry too much about this, since
/// there isn't that much time...
class PangolinGui {
public:
  PangolinGui(DynSlam *dyn_slam, Input *input)
      : dyn_slam_(dyn_slam),
        dyn_slam_input_(input)
  {
    cout << "Pangolin GUI initializing..." << endl;

    // TODO(andrei): Proper scaling to save space and memory.
    width_ = dyn_slam->GetInputWidth(); // / 1.5;
    height_ = dyn_slam->GetInputHeight();// / 1.5;

    SetupDummyImage();
    CreatePangolinDisplays();

    cout << "Pangolin GUI initialized." << endl;
  }

  virtual ~PangolinGui() {
    // No need to delete any view pointers; Pangolin deletes those itself on shutdown.
    delete dummy_image_texture_;
    delete pane_texture_;
  }

  /// \brief Executes the main Pangolin input and rendering loop.
  void Run() {
    // Default hooks for exiting (Esc) and fullscreen (tab).
    while (!pangolin::ShouldQuit()) {
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glColor3f(1.0, 1.0, 1.0);

      if (autoplay_->Get()) {
        ProcessFrame();
      }

      main_view_->Activate();
      glColor3f(1.0f, 1.0f, 1.0f);

      if(live_raycast_->Get()) {
        const unsigned char *slam_frame_data = dyn_slam_->GetRaycastPreview();
        pane_texture_->Upload(slam_frame_data, GL_RGBA, GL_UNSIGNED_BYTE);
      }
      else {
        // [RIP] If left unspecified, Pangolin assumes your texture type is single-channel luminance,
        // so you get dark, uncolored images.

        // Some experimental code for getting the camera to move on its own.
        if (wiggle_mode_->Get()) {
          struct timeval tp;
          gettimeofday(&tp, NULL);
          double time_ms = tp.tv_sec * 1000 + tp.tv_usec / 1000;
          double time_scale = 1500.0;
          double r = 0.2;
//          double cx = cos(time_ms / time_scale) * r;
          double cy = sin(time_ms / time_scale) * r - r * 2;
//          double cz = sin(time_ms / time_scale) * r;
          pane_cam_->SetModelViewMatrix(
              pangolin::ModelViewLookAt(
                  0, 0.0, 0.2,
                  0, 0.5 + cy, -1.0,
                  pangolin::AxisY)
          );
        }

        const unsigned char *preview = dyn_slam_->GetObjectRaycastFreeViewPreview(
            visualized_object_idx_,
            pane_cam_->GetModelViewMatrix());

        pane_texture_->Upload(
            preview,
            GL_RGBA,
            GL_UNSIGNED_BYTE);
      }
      pangolin::GlFont &font = pangolin::GlFont::I();
      pane_texture_->RenderToViewport(true);
      font.Text("Frame #%d", dyn_slam_->GetCurrentFrameNo()).Draw(-0.95, 0.9);

      rgb_view_.Activate();
      glColor3f(1.0f, 1.0f, 1.0f);
      if (display_raw_previews_->Get()) {
        LoadCvTexture(*(dyn_slam_->GetRgbPreview()), *pane_texture_);
      }
      else {
        LoadCvTexture(*(dyn_slam_->GetStaticRgbPreview()), *pane_texture_);
      }
      pane_texture_->RenderToViewport(true);

      if (dyn_slam_->GetCurrentFrameNo() > 1) {
        PreviewSparseSF(dyn_slam_->GetLatestFlow(),
                        rgb_view_.GetBounds().w,
                        rgb_view_.GetBounds().h);
      }

      depth_view_.Activate();
      glColor3f(1.0, 1.0, 1.0);
      if (display_raw_previews_->Get()) {
        LoadCvTexture(*(dyn_slam_->GetDepthPreview()), *pane_texture_, false, GL_SHORT);
      }
      else {
        LoadCvTexture(*(dyn_slam_->GetStaticDepthPreview()), *pane_texture_, false, GL_SHORT);
      }
      pane_texture_->RenderToViewport(true);

      segment_view_.Activate();
      glColor3f(1.0, 1.0, 1.0);
      if (nullptr != dyn_slam_->GetSegmentationPreview()) {
        LoadCvTexture(*dyn_slam_->GetSegmentationPreview(), *pane_texture_);
        pane_texture_->RenderToViewport(true);
        DrawInstanceLables();
      }

      object_view_.Activate();
      glColor3f(1.0, 1.0, 1.0);
      pane_texture_->Upload(dyn_slam_->GetObjectPreview(visualized_object_idx_),
                             GL_RGBA, GL_UNSIGNED_BYTE);
      pane_texture_->RenderToViewport(true);

      object_reconstruction_view_.Activate();
      glColor3f(1.0, 1.0, 1.0);
      pane_texture_->Upload(
          dyn_slam_->GetObjectRaycastPreview(
              visualized_object_idx_,
              instance_cam_->GetModelViewMatrix()
          ),
          GL_RGBA,
          GL_UNSIGNED_BYTE);
      pane_texture_->RenderToViewport(true);
      font.Text("Instance #%d", visualized_object_idx_).Draw(-1.0, 0.9);

      // Update various elements in the toolbar on the left.
      *(reconstructions) = Format(
        "%d active reconstructions",
        dyn_slam_->GetInstanceReconstructor()->GetActiveTrackCount()
      );

      // Disable autoplay once we reach the end of a sequence.
      if (! this->dyn_slam_input_->HasMoreImages()) {
        (*this->autoplay_) = false;
      }

      // Swap frames and Process Events
      pangolin::FinishFrame();
    }
  }

  /// \brief Renders informative labels regardin the currently active bounding boxes.
  /// Meant to be rendered over the segmentation preview window pane.
  void DrawInstanceLables() {
    pangolin::GlFont &font = pangolin::GlFont::I();

    auto &instanceTracker = dyn_slam_->GetInstanceReconstructor()->GetInstanceTracker();
    for (const auto &pair: instanceTracker.GetActiveTracks()) {
      Track &track = instanceTracker.GetTrack(pair.first);
      // Nothing to do for tracks with we didn't see this frame.
      if (track.GetLastFrame().frame_idx != dyn_slam_->GetCurrentFrameNo() - 1) {
        continue;
      }

      InstanceDetection latest_detection = track.GetLastFrame().instance_view.GetInstanceDetection();
      const auto &bbox = latest_detection.mask->GetBoundingBox();

      // Drawing the text requires converting from pixel coordinates to GL coordinates, which
      // range from (-1.0, -1.0) in the bottom-left, to (+1.0, +1.0) in the top-right.
      float panel_width = segment_view_.GetBounds().w;
      float panel_height = segment_view_.GetBounds().h;

      float bbox_left = bbox.r.x0 - panel_width;
      float bbox_top = panel_height - bbox.r.y0 + font.Height();

      float gl_x = bbox_left / panel_width;
      float gl_y = bbox_top / panel_height;

      stringstream info_label;
      info_label << latest_detection.GetClassName() << "#" << track.GetId()
                 << "@" << setprecision(2)
                 << latest_detection.class_probability;
      glColor3f(1.0f, 0.0f, 0.0f);
      font.Text(info_label.str()).Draw(gl_x, gl_y, 0);
    }
  }

  /// \brief Renders a simple preview of the scene flow information onto the currently active pane.
  void PreviewSparseSF(const SparseSceneFlow &flow, float panel_width, float panel_height) {
    pangolin::GlFont &font = pangolin::GlFont::I();
    font.Text("libviso2 scene flow preview").Draw(-0.98f, 0.89f);

    // We don't need z-checks since we're rendering UI stuff.
    glDisable(GL_DEPTH_TEST);
    for(const Matcher::p_match &match : flow.matches) {
      // TODO(andrei): uv-to-gl coordinate transformation utility.
      float gl_x = (match.u1c - panel_width) / panel_width;
      float gl_y = (panel_height - match.v1c) / panel_height;
      float gl_x_old = (match.u1p - panel_width) / panel_width;
      float gl_y_old = (panel_height - match.v1p) / panel_height;

      float delta_x = gl_x - gl_x_old;
      float delta_y = gl_y - gl_y_old;
      float magnitude = sqrt(delta_x * delta_x + delta_y * delta_y) * 15.0f;

      glColor4f(max(0.2f, min(1.0f, magnitude)), 0.4f, 0.4f, 1.0f);
      pangolin::glDrawCross(gl_x, gl_y, 0.025f);
      pangolin::glDrawLine(gl_x_old, gl_y_old, gl_x, gl_y);
    }

    glEnable(GL_DEPTH_TEST);
  }

protected:
  /// \brief Creates the GUI layout and widgets.
  /// \note The layout is biased towards very wide images (~2:1 aspect ratio or more), which is very
  /// common in autonomous driving datasets.
  void CreatePangolinDisplays() {
    pangolin::CreateWindowAndBind("DynSLAM GUI",
                                  kUiWidth + width_,
                                  // One full-height pane with the main preview, plus 3 * 0.5
                                  // height ones for various visualizations.
                                  static_cast<int>(ceil(height_ * 2.5)));

    // 3D Mouse handler requires depth testing to be enabled
    glEnable(GL_DEPTH_TEST);

    // Issue specific OpenGl we might need
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // GUI components
    pangolin::CreatePanel("ui")
      .SetBounds(0.0, 1.0, 0.0, pangolin::Attach::Pix(kUiWidth));

    pangolin::Var<function<void(void)>> a_button("ui.Save Static Map", [this]() {
      cout << "Saving static map..." << endl;
      dyn_slam_->SaveStaticMap(dyn_slam_input_->GetName(),
                               dyn_slam_input_->GetDepthProvider()->GetName());
      cout << "Done saving map." << endl;
    });
    reconstructions = new pangolin::Var<string>("ui.Rec", "");

    wiggle_mode_ = new pangolin::Var<bool>("ui.Wiggle mode", false, true);
    autoplay_ = new pangolin::Var<bool>("ui.Autoplay", false, true);
    live_raycast_ = new pangolin::Var<bool>("ui.Raycast Mode", false, true);
    display_raw_previews_ = new pangolin::Var<bool>("ui.Raw Previews", true, true);

    pangolin::Var<function<void(void)>> previous_object("ui.Previous Object", [this]() {
      SelectPreviousVisualizedObject();
    });
    pangolin::Var<function<void(void)>> next_object("ui.Next Object", [this]() {
      SelectNextVisualizedObject();
    });
    pangolin::RegisterKeyPressCallback('n', [this]() {
      *(this->autoplay_) = false;
      this->ProcessFrame();
    });
    pangolin::Var<function<void(void)>> save_object("ui.Save Active Object", [this]() {
      dyn_slam_->SaveDynamicObject(dyn_slam_input_->GetName(),
                                   dyn_slam_input_->GetDepthProvider()->GetName(),
                                   visualized_object_idx_);
    });
    pangolin::Var<function<void(void)>> quit_button("ui.Quit", []() {
      pangolin::QuitAll();
    });

    // This is used for the free view camera. The focal lengths are not used in rendering, BUT they
    // impact the sensitivity of the free view camera. The smaller they are, the faster the camera
    // responds to input.
    float cam_focal_length = 50.0f;
    proj_ = pangolin::ProjectionMatrix(width_, height_,
                                       cam_focal_length, cam_focal_length,
                                       width_ / 2, height_ / 2,
                                       0.1, 1000);
    pane_cam_ = new pangolin::OpenGlRenderState(
        proj_,
        pangolin::ModelViewLookAt(0, 0, 0,
                                  0, 0, -1,
                                  pangolin::AxisY));

    // TODO(andrei): Set dynamically based on where instances are detected (non-trivial).
    instance_cam_ = new pangolin::OpenGlRenderState(
        proj_,
        pangolin::ModelViewLookAt(
          0.0, 0.0, 0.75,
          -1.5, 0.0, -1.0,
          pangolin::AxisY)
    );

    float aspect_ratio = static_cast<float>(width_) / height_;
    rgb_view_ = pangolin::Display("rgb").SetAspect(aspect_ratio);
    depth_view_ = pangolin::Display("depth").SetAspect(aspect_ratio);

    segment_view_ = pangolin::Display("segment").SetAspect(aspect_ratio);
    object_view_ = pangolin::Display("object").SetAspect(aspect_ratio);
    object_reconstruction_view_ = pangolin::Display("object_3d").SetAspect(aspect_ratio)
        .SetHandler(new pangolin::Handler3D(*instance_cam_));

    // Storing pointers to these objects prevents a series of strange issues. The objects remain
    // under Pangolin's management, so they don't need to be deleted by the current class.
    main_view_ = &(pangolin::Display("main").SetAspect(aspect_ratio));
    main_view_->SetHandler(new pangolin::Handler3D(*pane_cam_));

    detail_views_ = &(pangolin::Display("detail"));

    // Add labels to our data logs (and automatically to our plots).
    data_log_.SetLabels({"Active tracks", "Free GPU Memory (100s of MiB)"});

    // OpenGL 'view' of data such as the number of actively tracked instances over time.
    float tick_x = 1.0f;
    float tick_y = 1.0f;
    plotter_ = new pangolin::Plotter(&data_log_, 0.0f, 200.0f, -0.1f, 25.0f, tick_x, tick_y);
    plotter_->Track("$i");  // This enables automatic scrolling for the live plots.

    // TODO(andrei): Maybe wrap these guys in another controller, make it an equal layout and
    // automagically support way more aspect ratios?
    main_view_->SetBounds(pangolin::Attach::Pix(height_ * 1.5), 1.0,
                         pangolin::Attach::Pix(kUiWidth), 1.0);
    detail_views_->SetBounds(0.0, pangolin::Attach::Pix(height_ * 1.5),
                            pangolin::Attach::Pix(kUiWidth), 1.0);
    detail_views_->SetLayout(pangolin::LayoutEqual)
      .AddDisplay(rgb_view_)
      .AddDisplay(depth_view_)
      .AddDisplay(segment_view_)
      .AddDisplay(object_view_)
      .AddDisplay(*plotter_)
      .AddDisplay(object_reconstruction_view_);

    // Internally, InfiniTAM stores these as RGBA, but we discard the alpha when we upload the
    // textures for visualization (hence the 'GL_RGB' specification).
    this->pane_texture_ = new pangolin::GlTexture(width_, height_, GL_RGB, false, 0, GL_RGB, GL_UNSIGNED_BYTE);
    cout << "Pangolin UI setup complete." << endl;
  }

  void SetupDummyImage() {
    dummy_img_ = cv::imread("../data/george.jpg");
//    cv::flip(dummy_img_, dummy_img_, kCvFlipVertical);
    const int george_width = dummy_img_.cols;
    const int george_height = dummy_img_.rows;
    this->dummy_image_texture_ = new pangolin::GlTexture(
      george_width, george_height, GL_RGB, false, 0, GL_RGB, GL_UNSIGNED_BYTE);
  }

  void SelectNextVisualizedObject() {
    // We pick the closest next object (by ID). We need to do this because some tracks may no
    // longer be available.
    InstanceTracker &tracker = dyn_slam_->GetInstanceReconstructor()->GetInstanceTracker();
    int closest_next_id = -1;
    int closest_next_delta = std::numeric_limits<int>::max();

    if (tracker.GetActiveTrackCount() == 0) {
      visualized_object_idx_ = 0;
      return;
    }

    for(const auto &pair : tracker.GetActiveTracks()) {
      int id = pair.first;
      int delta = id - visualized_object_idx_;
      if (delta < closest_next_delta && delta != 0) {
        closest_next_delta = delta;
        closest_next_id = id;
      }
    }

    visualized_object_idx_ = closest_next_id;
  }

  void SelectPreviousVisualizedObject() {
    int active_tracks = dyn_slam_->GetInstanceReconstructor()->GetActiveTrackCount();
    if (visualized_object_idx_ <= 0) {
      visualized_object_idx_ = active_tracks - 1;
    }
    else {
      visualized_object_idx_--;
    }
  }

  /// \brief Advances to the next input frame, and integrates it into the map.
  void ProcessFrame() {
    active_object_count_ = dyn_slam_->GetInstanceReconstructor()->GetActiveTrackCount();

    size_t free_gpu_memory_bytes;
    size_t total_gpu_memory_bytes;
    cudaMemGetInfo(&free_gpu_memory_bytes, &total_gpu_memory_bytes);

    const double kBytesToGb = 1.0 / 1024.0 / 1024.0 / 1024.0;
    double free_gpu_gb = static_cast<float>(free_gpu_memory_bytes) * kBytesToGb;
    data_log_.Log(
        active_object_count_,
        static_cast<float>(free_gpu_gb) * 10.0f    // Mini-hack to make the scales better
    );

    // Main workhorse function of the underlying SLAM system.
    dyn_slam_->ProcessFrame(this->dyn_slam_input_);
  }

private:
  DynSlam *dyn_slam_;
  Input *dyn_slam_input_;

  /// Input frame dimensions. They dictate the overall window size.
  int width_, height_;

  pangolin::View *main_view_;
  pangolin::View *detail_views_;
  pangolin::View rgb_view_;
  pangolin::View depth_view_;
  pangolin::View segment_view_;
  pangolin::View object_view_;
  pangolin::View object_reconstruction_view_;

  pangolin::OpenGlMatrix proj_;
  pangolin::OpenGlRenderState *pane_cam_;
  pangolin::OpenGlRenderState *instance_cam_;

  // Graph plotter and its data logger object
  pangolin::Plotter *plotter_;
  pangolin::DataLog data_log_;

  pangolin::GlTexture *dummy_image_texture_;
  pangolin::GlTexture *pane_texture_;

  pangolin::Var<string> *reconstructions;

  /// \brief Used for UI testing whenever necessary. Not related to the SLAM system in any way.
  cv::Mat dummy_img_;

  // Atomic because it gets set from a UI callback. Technically, Pangolin shouldn't invoke callbacks
  // from a different thread, but using atomics for this is generally a good practice anyway.
  atomic<int> active_object_count_;

  /// \brief Whether the 3D scene view should be automatically moving around.
  /// If this is off, then the user has control over the camera.
  pangolin::Var<bool> *wiggle_mode_;
  /// \brief When this is on, the input gets processed as fast as possible, without requiring any
  /// user input.
  pangolin::Var<bool> *autoplay_;
  /// \brief If enabled, the latest frame's raycast is displayed in the main pane. Otherwise, a
  /// free-roam view of the scene is provided.
  pangolin::Var<bool> *live_raycast_;
  /// \brief Whether to display the RGB and depth previews directly from the input, or from the
  /// static scene, i.e., with the dynamic objects removed.
  pangolin::Var<bool> *display_raw_previews_;

  // TODO(andrei): On-the-fly depth provider toggling.
  // TODO(andrei): Reset button.
  // TODO(andrei): Dynamically set depth range.

  // Indicates which object is currently being visualized in the GUI.
  int visualized_object_idx_ = 0;

  /// \brief Prepares the contents of an OpenCV Mat object for rendering with Pangolin (OpenGL).
  /// Does not actually render the texture.
  static void LoadCvTexture(
      const cv::Mat &mat,
      pangolin::GlTexture &texture,
      bool color = true,
      GLenum data_type = GL_UNSIGNED_BYTE
  ) {
    int old_alignment, old_row_length;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &old_row_length);

    int new_alignment = (mat.step & 3) ? 1 : 4;
    int new_row_length = static_cast<int>(mat.step / mat.elemSize());
    glPixelStorei(GL_UNPACK_ALIGNMENT, new_alignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, new_row_length);

    GLenum data_format = (color) ? GL_BGR : GL_GREEN;
    texture.Upload(mat.data, data_format, data_type);

    glPixelStorei(GL_UNPACK_ALIGNMENT, old_alignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, old_row_length);
  }

  void UploadDummyTexture() {
    // Mess with the bytes a little bit for OpenGL <-> OpenCV compatibility.
    // use fast 4-byte alignment (default anyway) if possible
    glPixelStorei(GL_UNPACK_ALIGNMENT, (dummy_img_.step & 3) ? 1 : 4);
    //set length of one complete row in data (doesn't need to equal img.cols)
    glPixelStorei(GL_UNPACK_ROW_LENGTH, dummy_img_.step/dummy_img_.elemSize());
    dummy_image_texture_->Upload(dummy_img_.data, GL_BGR, GL_UNSIGNED_BYTE);
  }
};

} // namespace gui

/// \brief Constructs a DynSLAM instance to run on a KITTI Odometry dataset sequence, using the
///        ground truth pose information from the Inertial Navigation System (INS) instead of any
///        visual odometry.
/// This is useful when you want to focus on the quality of the reconstruction, instead of that of
/// the odometry.
void BuildDynSlamKittiOdometryGT(const string &dataset_root, DynSlam **dyn_slam_out, Input **input_out) {
  // Parameters used in the KITTI-odometry dataset.
  // TODO(andrei): Read from a file.
  float baseline_m = 0.537150654273f;
  float focal_length_px = 707.0912f;
  StereoCalibration stereo_calibration(baseline_m, focal_length_px);

  Input::Config input_config = Input::KittiOdometryConfig();
  auto itm_calibration = ReadITMCalibration(dataset_root + "/" + input_config.itm_calibration_fname);

  *input_out = new Input(
      dataset_root,
      input_config,
      // TODO(andrei): ¿Por qué no los dos? Maybe it's worth investigating to use the more
      // conservative, sharper libelas depth maps for the main map, but the smoother ones produced
      // by dispnet for the objects. This may be a good idea since libelas tends to screw up when
      // faced with reflective surfaces, but dispnet is more robust to that. Similarly, for certain
      // areas of the static map such as foliage and fences, dispnet's smoothness is more of a
      // liability than an asset.
      // TODO(andrei): Make sure you normalize dispnet's depth range when using it, since it seems
      // to be inconsistent across frames.
      // TODO(andrei): Carefully read the dispnet paper.
      new PrecomputedDepthProvider(dataset_root + "/" + input_config.depth_folder,
                                   input_config.depth_fname_format,
                                   input_config.read_depth),
      itm_calibration,
      stereo_calibration);

  // [RIP] I lost a couple of hours debugging a bug caused by the fact that InfiniTAM still works
  // even when there is a discrepancy between the size of the depth/rgb inputs, as specified in the
  // calibration file, and the actual size of the input images.

  ITMLibSettings *settings = new ITMLibSettings();
  settings->groundTruthPoseFpath = dataset_root + "/" + input_config.odometry_fname;

  drivers::InfiniTamDriver *driver = new InfiniTamDriver(
      settings,
      new ITMRGBDCalib(itm_calibration),
      ToItmVec((*input_out)->GetRgbSize()),
      ToItmVec((*input_out)->GetDepthSize()));

  const string seg_folder = dataset_root + "/" + input_config.segmentation_folder;
  auto segmentation_provider =
      new instreclib::segmentation::PrecomputedSegmentationProvider(seg_folder);

  VisualOdometryStereo::parameters sf_params;
  sf_params.base = baseline_m;
//  sf_params.ransac_iters = 50;
//  sf_params.match.refinement = 0;
  sf_params.calib.cu = itm_calibration.intrinsics_rgb.projectionParamsSimple.px;
  sf_params.calib.cv = itm_calibration.intrinsics_rgb.projectionParamsSimple.py;
  sf_params.calib.f  = itm_calibration.intrinsics_rgb.projectionParamsSimple.fx; // TODO should we average fx and fy?

  auto sparse_sf_provider = new instreclib::VisoSparseSFProvider(sf_params);

  *dyn_slam_out = new gui::DynSlam();
  (*dyn_slam_out)->Initialize(driver, segmentation_provider, sparse_sf_provider);
}

} // namespace dynslam

int main(int argc, char **argv) {
  using namespace dynslam;
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  const string dataset_root = FLAGS_dataset_root;
  if (dataset_root.empty()) {
    cerr << "Please specify a dataset to work with. The --dataset_root=<path> flag must be set."
         << endl;

    return -1;
  }

  DynSlam *dyn_slam;
  Input *input;
  BuildDynSlamKittiOdometryGT(dataset_root, &dyn_slam, &input);

  gui::PangolinGui pango_gui(dyn_slam, input);
  pango_gui.Run();

  delete dyn_slam;
  delete input;
}


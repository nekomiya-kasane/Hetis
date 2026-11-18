// #pragma once


// #include <ImpellerHeader.h>

// namespace Ui {

//     class Picture;

//     template<typename T>
//     using RefCounted = Sora::RefCounted<T>;

//     class PictureRecorder : public RefCounted<PictureRecorder> {
//     public:
//         static void Create();

//         virtual ~PictureRecorder();

//         std::shared_ptr<DisplayListBuilder> BeginRecording(ImpellerRect bounds);
//         void endRecording(RefCounted<Picture> picture);

//         void set_canvas(RefPtr<Canvas> canvas) { canvas_ = std::move(canvas); }

//     private:
//         PictureRecorder();

//         sk_sp<DisplayListBuilder> display_list_builder_;

//         fml::RefPtr<Canvas> canvas_;
//     };

// } // namespace Ui
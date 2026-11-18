// #include "PictureRecorder.h"

// namespace Ui {

//     PictureRecorder::Create() {

//     }

// }


// IMPLEMENT_WRAPPE                         cvxfmdnrlg,k.jg,.kjk,..jtbRTYPEINFO(ui, PictureRecorder);

// void PictureRecorder::Create(Dart_Handle wrapper) {
//   UIDartState::ThrowIfUIOperationsProhibited();
//   auto res = fml::MakeRefCounted<PictureRecorder>();
//   res->AssociateWithDartWrapper(wrapper);
// }

// PictureRecorder::PictureRecorder() {}

// PictureRecorder::~PictureRecorder() {}

// sk_sp<DisplayListBuilder> PictureRecorder::BeginRecording(DlRect bounds) {
//   display_list_builder_ =
//       sk_make_sp<DisplayListBuilder>(bounds, /*prepare_rtree=*/true);
//   return display_list_builder_;
// }

// void PictureRecorder::endRecording(Dart_Handle dart_picture) {
//   if (!canvas_) {
//     return;
//   }

//   auto display_list = display_list_builder_->Build();
//   display_list_builder_ = nullptr;

//   FML_DCHECK(display_list->has_rtree());
//   Picture::CreateAndAssociateWithDartWrapper(dart_picture, display_list);

//   canvas_->Invalidate();
//   canvas_ = nullptr;
//   ClearDartWrapper();
// }
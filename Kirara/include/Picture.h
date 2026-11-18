// #pragma once

// #include <Sora/Core/RefCounted.h>
// #include <ImpellerHeader.h>

// namespace Ui {

//     class Canvas;

//     class Picture : public RefCounted<Picture> {
//     public:
//         ~Picture() override;
//         static void Create(std::shared_ptr<DisplayList> display_list);

//         std::shared_ptr<DisplayList> display_list() const { return display_list_; }

//         Dart_Handle toImage(uint32_t width, uint32_t height, Dart_Handle raw_image_callback);

//         void toImageSync(uint32_t width, uint32_t height, Dart_Handle raw_image_handle);

//         void dispose();

//         size_t GetAllocationSize() const;

//         static void RasterizeToImageSync(std::shared_ptr<DisplayList> display_list, uint32_t width, uint32_t height,
//                                          Dart_Handle raw_image_handle);

//         static Dart_Handle RasterizeToImage(const std::shared_ptr<DisplayList>& display_list, uint32_t width, uint32_t height,
//                                             Dart_Handle raw_image_callback);

//         static Dart_Handle RasterizeLayerTreeToImage(std::unique_ptr<LayerTree> layer_tree,
//                                                      Dart_Handle raw_image_callback);

//         // Callers may provide either a display list or a layer tree, but not both.
//         //
//         // If a layer tree is provided, it will be flattened on the raster thread, and
//         // picture_bounds should be the layer tree's frame_size().
//         static Dart_Handle DoRasterizeToImage(const std::shared_ptr<DisplayList>& display_list,
//                                               std::unique_ptr<LayerTree> layer_tree, uint32_t width, uint32_t height,
//                                               Dart_Handle raw_image_callback);

//     private:
//         explicit Picture(std::shared_ptr<DisplayList> display_list);

//         std::shared_ptr<DisplayList> display_list_;
//     };

// } // namespace Ui

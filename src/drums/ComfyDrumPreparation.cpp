#include "ComfyDrumPreparation.h"
namespace airack::drums {
namespace {
class HeldComfyBytes final : public GeneratedDrumBytes {
public:
 explicit HeldComfyBytes(companion::ComfyRawFile& raw) noexcept:borrowed(&raw){}
 std::uint64_t size() const noexcept override{return file().size();}
 bool readAt(std::uint64_t offset,std::span<std::byte> bytes) const noexcept override{return file().readAt(offset,bytes);}
 void adopt(std::unique_ptr<companion::ComfyRawFile> raw) noexcept{owner=std::move(raw);borrowed=nullptr;}
private:
 const companion::ComfyRawFile& file() const noexcept{return owner?*owner:*borrowed;}
 std::unique_ptr<companion::ComfyRawFile> owner;
 companion::ComfyRawFile* borrowed;
};
}
bool inspectComfyDrumSource(std::unique_ptr<companion::ComfyRawFile>& raw,
    const GeneratedOrigin& origin,const std::atomic_bool& cancel,
    std::unique_ptr<DrumSource>& destination,PreparationError& error) noexcept {
 error=PreparationError::none;
 if(cancel.load()){error=PreparationError::cancelled;return false;}
 if(!raw){error=PreparationError::identityMismatch;return false;}
 for(std::size_t n=0;n<32;++n)if(origin.expectedFileId[n]!=std::to_integer<std::uint8_t>(raw->sha256()[n])){error=PreparationError::identityMismatch;return false;}
 try{
  auto wrapper=std::make_unique<HeldComfyBytes>(*raw);auto* held=wrapper.get();
  std::unique_ptr<GeneratedDrumBytes> reader=std::move(wrapper);
  std::unique_ptr<DrumSource> candidate;
  if(!inspectDrumSource(reader,origin,cancel,candidate,error))return false;
  if(cancel.load()){error=PreparationError::cancelled;return false;}
  // Borrow until all fallible inspection and both destination shells exist.
  held->adopt(std::move(raw));destination.swap(candidate);return true;
 }catch(const std::bad_alloc&){error=cancel.load()?PreparationError::cancelled:PreparationError::allocationFailed;return false;}
}
}

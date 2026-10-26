//! Backend-independent EDRAM transfer uniform and clip contract. Addresses are
//! guest tile identities; only pixel coordinates are resolution-scaled.
use xenos_state::{edram::{RenderTargetKey,EDRAM_TILE_COUNT},xenos::{ColorRenderTargetFormat as C,DepthRenderTargetFormat as D}};

#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum ColorStorage {Rgba8,Rgb10A2,Rg16Float,Rgba16Float,R32Float,Rg32Float}
pub fn color_storage(format:C)->Option<ColorStorage>{Some(match format.storage_format(){
 C::K8888=>ColorStorage::Rgba8,C::K2101010=>ColorStorage::Rgb10A2,
 C::K8888Gamma|C::K2101010Float|C::K16161616|C::K16161616Float=>ColorStorage::Rgba16Float,
 C::K1616|C::K1616Float=>ColorStorage::Rg16Float,C::K32Float=>ColorStorage::R32Float,C::K3232Float=>ColorStorage::Rg32Float,_=>return None,
})}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub enum Error{Scale,Key,Extent,Range,Cutout}

#[derive(Clone,Debug)]
pub struct Transfer {
 source:RenderTargetKey,destination:RenderTargetKey,source_extent:[u32;2],range:[u32;2],cutout:[u32;4],scale:u32,
}
fn check_key(k:RenderTargetKey)->bool {
 k.base_tiles<EDRAM_TILE_COUNT && (1..=1024).contains(&k.pitch_tiles_at_32bpp) && k.msaa_samples.count().is_some()
 && if k.is_depth{matches!(k.depth_format(),D::D24S8|D::D24FS8)}else{color_storage(k.color_format()).is_some() && k.color_format().storage_format().raw()==k.format}
}
impl Transfer {
 /// `range` is (guest starting tile, tile count); `cutout` is a guest-pixel
 /// rectangle [left,top,right,bottom]. The source extent is in HOST pixels.
 pub fn new(source:RenderTargetKey,destination:RenderTargetKey,source_extent:[u32;2],range:[u32;2],cutout:Option<[u32;4]>,scale:u32)->Result<Self,Error>{
  if !(1..=3).contains(&scale){return Err(Error::Scale);}
  if !check_key(source)||!check_key(destination){return Err(Error::Key);}
  if source_extent.iter().any(|&n|n==0||n>24576){return Err(Error::Extent);}
  if range[0]>=EDRAM_TILE_COUNT||range[1]>EDRAM_TILE_COUNT{return Err(Error::Range);}
  let cutout=cutout.unwrap_or([0;4]);
  if cutout[0]>cutout[2]||cutout[1]>cutout[3]||cutout.iter().any(|&v|v>8192){return Err(Error::Cutout);}
  Ok(Self{source,destination,source_extent,range,cutout,scale})
 }
 pub fn source(&self)->RenderTargetKey{self.source}
 pub fn destination(&self)->RenderTargetKey{self.destination}
 pub fn source_extent(&self)->[u32;2]{self.source_extent}
 pub fn scale(&self)->u32{self.scale}
 pub fn words(&self)->[u32;20]{
  let flags=|k:RenderTargetKey|u32::from(k.is_depth)|(u32::from(k.is_64bpp())<<1);
  let format=|k:RenderTargetKey|k.format+if k.is_depth{16}else{0};let d=self.destination;let s=self.source;let cut=self.cutout.map(|v|v*self.scale);
  [d.base_tiles,d.pitch_tiles(),d.msaa_samples.raw(),flags(d),format(d),s.base_tiles,s.pitch_tiles(),s.msaa_samples.raw(),flags(s),format(s),
   self.source_extent[0],self.source_extent[1],self.range[0],self.range[1],0,0,cut[0],cut[1],cut[2],cut[3]]
 }
 pub fn scissor(&self,width:u32,height:u32)->Result<Option<[u32;4]>,Error>{
  if width==0||height==0||width>24576||height>24576{return Err(Error::Extent);}
  if self.range[1]==0{return Ok(None);}
  let d=self.destination;let start=self.range[0].wrapping_sub(d.base_tiles)&(EDRAM_TILE_COUNT-1);let end=start+self.range[1];
  let tile_h=(16>>d.msaa_samples.samples_y_log2())*self.scale;
  let (y0,y1)=if end>EDRAM_TILE_COUNT{(0,height)}else{((start/d.pitch_tiles())*tile_h,((end-1)/d.pitch_tiles()+1)*tile_h)};
  let y1=y1.min(height);Ok((y0<y1).then_some([0,y0,width,y1.saturating_sub(y0)]))
 }
}

#[cfg(test)] mod tests{
 use super::*;use xenos_state::xenos::MsaaSamples;
 fn key(base:u32)->RenderTargetKey{RenderTargetKey{base_tiles:base,pitch_tiles_at_32bpp:2,msaa_samples:MsaaSamples::X4,is_depth:false,format:0}}
 #[test] fn transfer_uniform_preserves_tile_units_and_scales_only_pixel_cutouts(){
  let t=Transfer::new(key(2047),key(0),[160,96],[0,2],Some([2,3,9,10]),3).unwrap();let words=t.words();
  assert_eq!(&words[0..5],&[0,2,2,0,0]);assert_eq!(&words[5..10],&[2047,2,2,0,0]);assert_eq!(&words[10..],&[160,96,0,2,0,0,6,9,27,30]);
  assert_eq!(t.scissor(160,96),Ok(Some([0,0,160,24])));
  let wrapped=Transfer::new(key(0),key(2047),[80,32],[2046,3],None,1).unwrap();assert_eq!(wrapped.scissor(80,32),Ok(Some([0,0,80,32])));
  let empty=Transfer::new(key(0),key(0),[80,32],[0,0],None,1).unwrap();assert_eq!(empty.scissor(80,32),Ok(None));
 }
 #[test] fn malformed_transfer_is_rejected_before_gpu_resources_exist(){
  for scale in [0,4,u32::MAX]{assert_eq!(Transfer::new(key(0),key(0),[80,32],[0,1],None,scale).unwrap_err(),Error::Scale);}
  for k in [RenderTargetKey{pitch_tiles_at_32bpp:0,..key(0)},RenderTargetKey{format:99,..key(0)},key(2048)]{assert_eq!(Transfer::new(k,key(0),[80,32],[0,1],None,1).unwrap_err(),Error::Key);}
  assert_eq!(Transfer::new(key(0),key(0),[80,32],[0,2049],None,1).unwrap_err(),Error::Range);
  assert_eq!(Transfer::new(key(0),key(0),[80,32],[0,1],Some([9,3,2,4]),1).unwrap_err(),Error::Cutout);
 }
}

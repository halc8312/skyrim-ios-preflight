//! CPU comparison path for the public binned constant-patch format. This is
//! deliberately not a production default: uploading expanded blocks may cost
//! more bandwidth than GPU expansion on Apple hardware. Values remain bitwise
//! bytes (including NaNs and signed zero), never floating-point arithmetic.

pub const BINNED_SHADER: &str = include_str!("constant_patch.wgsl");

#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub struct ConstantError(pub &'static str);
#[derive(Clone,Copy,Debug,Default)]
pub struct MaterializeStats {pub records:u32,pub changed_bytes:u64,pub written_bytes:u64}
pub struct ConstantPlan<'a> {records:&'a [u32],values:&'a [u8],output_length:usize,stats:MaterializeStats}
impl<'a> ConstantPlan<'a> {
    /// Freeze validated inputs before touching output. Ordered, nonoverlapping
    /// destinations match ConstantPatcher's append-only allocation contract.
    /// Inputs outside this supported contract must stay on the GPU path.
    pub fn validate(records:&'a [u32],values:&'a [u8],output_length:usize)->Result<Self,ConstantError> {
        let bad=|reason|Err(ConstantError(reason));
        if records.len()<16 || records[0]>16384 || values.len()>((1<<17)*16) || !values.len().is_multiple_of(16)
            || output_length>16<<20 || !output_length.is_multiple_of(16) {return bad("constant input bounds/alignment");}
        let count=records[0] as usize;let body=16+count*12;
        if records.len()<body {return bad("truncated constant records");}
        let mut counts=[0u32;5];let mut maxima=[0u32;5];let mut previous_end=0u64;
        let mut stats=MaterializeStats{records:count as u32,..Default::default()};
        for r in records[16..body].chunks_exact(12) {
            let stage=r[0] as usize;let n=r[2];
            if stage>=5 || n>256 {return bad("constant stage/length");}
            let begin=u64::from(r[1])*16;let end=begin+u64::from(n)*16;
            if end>output_length as u64 || n!=0 && begin<previous_end {return bad("overlapping/unordered/outside constant output");}
            if n!=0 {previous_end=end;}
            let changed=r[4..12].iter().map(|m|m.count_ones()).sum::<u32>();
            if u64::from(r[3])+u64::from(changed)>(values.len()/16) as u64 {return bad("constant changed value range");}
            counts[stage]+=1;maxima[stage]=maxima[stage].max(n);
            stats.changed_bytes+=u64::from(changed)*16;stats.written_bytes+=u64::from(n)*16;
        }
        for stage in 0..5 {
            let h=&records[1+stage*3..4+stage*3];
            if h[1]!=counts[stage] || h[2]!=maxima[stage] {return bad("constant bin count/maximum mismatch");}
            let first=h[0] as usize;let Some(end)=first.checked_add(h[1] as usize) else{return bad("constant bin overflow")};
            if end>records.len() || h[1]!=0 && first<body {return bad("constant bin range");}
            let mut previous=None;
            for &index in &records[first..end] {
                if index>=count as u32 || previous.is_some_and(|p|index<=p)
                    || records[16+index as usize*12]!=stage as u32 {return bad("constant bin ordering/coverage");}
                previous=Some(index);
            }
        }
        Ok(Self{records,values,output_length,stats})
    }
    pub fn stats(&self)->MaterializeStats {self.stats}
    pub fn execute(&self,output:&mut [u8])->Result<MaterializeStats,ConstantError> {
        if output.len()!=self.output_length {return Err(ConstantError("constant output length changed"));}
        for stage in 0..5 {
            // Stages write disjoint output ranges. Process one 4KiB state at a
            // time, in its validated original record order; no per-record heap.
            let mut state=[[0u8;16];256];let h=&self.records[1+stage*3..4+stage*3];
            for &index in &self.records[h[0] as usize..h[0] as usize+h[1] as usize] {
                let r=&self.records[16+index as usize*12..][..12];let mut value=r[3] as usize*16;
                for (word,&bits) in r[4..12].iter().enumerate() {
                    let mut bits=bits;
                    while bits!=0 {
                        let lane=word*32+bits.trailing_zeros() as usize;
                        state[lane].copy_from_slice(&self.values[value..value+16]);value+=16;bits&=bits-1;
                    }
                }
                let start=r[1] as usize*16;let count=r[2] as usize*16;
                output[start..start+count].copy_from_slice(&state.as_flattened()[..count]);
            }
        }
        Ok(self.stats)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn one()->(Vec<u32>,Vec<u8>) {
        let mut r=vec![0;29];r[0]=1;r[1..4].copy_from_slice(&[28,1,1]);
        for stage in 1..5 {r[1+stage*3]=29;}
        r[16..20].copy_from_slice(&[0,0,1,0]);r[20]=1;r[28]=0;
        (r,[0x7fc00001u32,0x80000000,123,999].into_iter().flat_map(u32::to_le_bytes).collect())
    }
    #[test]
    fn malformed_inputs_cannot_change_output_and_execute_checks_its_validated_length() {
        let (r,v)=one();let mut dst=[0xA5;32];
        for (index,value) in [(0,16385),(1,16),(2,0),(3,2),(16,5),(17,u32::MAX),(18,257),(19,1),(28,1)] {
            let mut invalid=r.clone();invalid[index]=value;
            assert!(ConstantPlan::validate(&invalid,&v,dst.len()).is_err(),"word{index}");assert_eq!(dst,[0xA5;32]);
        }
        let p=ConstantPlan::validate(&r,&v,dst.len()).unwrap();assert!(p.execute(&mut dst[..16]).is_err());assert_eq!(dst,[0xA5;32]);
        p.execute(&mut dst).unwrap();assert_eq!(&dst[..16],&v);assert_eq!(&dst[16..],&[0xA5;16]);
    }
    #[test]
    fn changed_high_lane_survives_short_blocks_without_overwriting_padding() {
        let mut records=vec![0u32;16+3*12+3];records[0]=3;records[1..4].copy_from_slice(&[52,3,256]);
        for stage in 1..5 {records[1+3*stage]=55;}
        records[16..20].copy_from_slice(&[0,0,1,0]);records[27]=1<<31;
        records[28..32].copy_from_slice(&[0,16,0,1]);
        records[40..44].copy_from_slice(&[0,32,256,1]);records[52..].copy_from_slice(&[0,1,2]);
        let values=[0x7Fu8;16];let mut dst=vec![0xA5;512+4096];
        let stats=ConstantPlan::validate(&records,&values,dst.len()).unwrap().execute(&mut dst).unwrap();
        assert_eq!(&dst[..16],&[0;16]);assert!(dst[16..512].iter().all(|&v|v==0xA5));
        assert!(dst[512..512+255*16].iter().all(|&v|v==0));assert_eq!(&dst[512+255*16..],&values);
        assert_eq!((stats.records,stats.changed_bytes,stats.written_bytes),(3,16,4112));
    }
}

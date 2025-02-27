; Test to verify that an outlined helper function discards the
; "returned" attribute on function parameters.
;
; Credit to I-Ting Angelina Lee for the original source code for this
; test.
;
; RUN: opt < %s -passes=tapir2target -tapir-target=opencilk -opencilk-runtime-bc-path=%S/libopencilk-abi.bc -S | FileCheck %s

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@merge_grain = internal unnamed_addr global i64 0, align 8

; Function Attrs: nounwind uwtable
define i64* @cilk_merge(i64* nocapture readonly %source_1, i64 %size_1, i64* nocapture readonly %source_2, i64 %size_2, i64* returned %target) local_unnamed_addr #0 {
entry:
  %0 = load i64, i64* @merge_grain, align 8, !tbaa !2
  br label %tailrecurse

tailrecurse:                                      ; preds = %if.end, %entry
  %source_1.tr = phi i64* [ %source_1, %entry ], [ %source_2.tr, %if.end ]
  %size_1.tr = phi i64 [ %size_1, %entry ], [ %size_2.tr, %if.end ]
  %source_2.tr = phi i64* [ %source_2, %entry ], [ %source_1.tr, %if.end ]
  %size_2.tr = phi i64 [ %size_2, %entry ], [ %size_1.tr, %if.end ]
  %syncreg = tail call token @llvm.syncregion.start()
  %add = add nsw i64 %size_2.tr, %size_1.tr
  %cmp = icmp slt i64 %add, %0
  br i1 %cmp, label %if.then, label %if.end

if.then:                                          ; preds = %tailrecurse
  %call = tail call i64* @seq_merge(i64* %source_1.tr, i64 %size_1.tr, i64* %source_2.tr, i64 %size_2.tr, i64* %target)
  br label %det.cont.split

if.end:                                           ; preds = %tailrecurse
  %cmp1 = icmp slt i64 %size_1.tr, %size_2.tr
  br i1 %cmp1, label %tailrecurse, label %if.end4

if.end4:                                          ; preds = %if.end
  %cmp5 = icmp slt i64 %size_2.tr, 1
  br i1 %cmp5, label %if.then6, label %if.end7

if.then6:                                         ; preds = %if.end4
  %1 = bitcast i64* %target to i8*
  %2 = bitcast i64* %source_1.tr to i8*
  %mul = shl i64 %size_1.tr, 3
  tail call void @llvm.memcpy.p0i8.p0i8.i64(i8* %1, i8* %2, i64 %mul, i32 8, i1 false)
  br label %det.cont.split

if.end7:                                          ; preds = %if.end4
  %div = sdiv i64 %size_1.tr, 2
  %sub = add nsw i64 %div, -1
  %arrayidx = getelementptr inbounds i64, i64* %source_1.tr, i64 %sub
  %3 = load i64, i64* %arrayidx, align 8, !tbaa !2
  %call8 = tail call i64 @binary_search(i64* %source_2.tr, i64 %size_2.tr, i64 %3)
  %add9 = add nsw i64 %call8, 1
  detach within %syncreg, label %det.achd, label %det.cont
; CHECK: define internal fastcc void @cilk_merge.outline_det.achd.otd1(ptr align 1 %source_1.tr.otd1, i64 %div.otd1, ptr align 1 %source_2.tr.otd1, i64 %add9.otd1, ptr
; CHECK-NOT: returned
; CHECK: %target.otd1) unnamed_addr
det.achd:                                         ; preds = %if.end7
  %call10 = tail call i64* @cilk_merge(i64* nonnull %source_1.tr, i64 %div, i64* %source_2.tr, i64 %add9, i64* %target)
  reattach within %syncreg, label %det.cont

det.cont:                                         ; preds = %det.achd, %if.end7
  %add.ptr = getelementptr inbounds i64, i64* %source_1.tr, i64 %div
  %sub11 = sub nsw i64 %size_1.tr, %div
  %add.ptr12 = getelementptr inbounds i64, i64* %source_2.tr, i64 %add9
  %sub13 = sub nsw i64 %size_2.tr, %add9
  %add14 = add nsw i64 %add9, %div
  %add.ptr15 = getelementptr inbounds i64, i64* %target, i64 %add14
  %call16 = tail call i64* @cilk_merge(i64* %add.ptr, i64 %sub11, i64* %add.ptr12, i64 %sub13, i64* %add.ptr15)
  sync within %syncreg, label %det.cont.split

det.cont.split:                                   ; preds = %if.then, %if.then6, %det.cont
  ret i64* %target
}

; Function Attrs: argmemonly nounwind
declare token @llvm.syncregion.start() #3

declare i64* @seq_merge(i64* nocapture readonly %source_1, i64 %size_1, i64* nocapture readonly %source_2, i64 %size_2, i64* returned %target) local_unnamed_addr #0

declare i64 @binary_search(i64* nocapture readonly %array, i64 %size, i64 %target) local_unnamed_addr #4

; Function Attrs: argmemonly nounwind
declare void @llvm.memcpy.p0i8.p0i8.i64(i8* nocapture writeonly, i8* nocapture readonly, i64, i32, i1) #3

attributes #0 = { nounwind uwtable "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-jump-tables"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="false" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+fxsr,+mmx,+sse,+sse2,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { nounwind "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="false" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+fxsr,+mmx,+sse,+sse2,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #2 = { noreturn nounwind "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="false" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+fxsr,+mmx,+sse,+sse2,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #3 = { nounwind stealable uwtable "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-jump-tables"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="false" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+fxsr,+mmx,+sse,+sse2,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #4 = { norecurse nounwind readonly uwtable "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-jump-tables"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="false" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+fxsr,+mmx,+sse,+sse2,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #5 = { argmemonly nounwind }
attributes #6 = { noinline nounwind uwtable "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-jump-tables"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="false" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+fxsr,+mmx,+sse,+sse2,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #7 = { nounwind readnone }
attributes #8 = { nounwind }
attributes #9 = { noreturn nounwind }
attributes #10 = { returns_twice }
attributes #11 = { nounwind returns_twice }

!llvm.module.flags = !{!0}
!llvm.ident = !{!1}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{!"clang version 5.0.0 (git@github.com:wsmoses/Cilk-Clang.git 51d7b71ff6cb4c026e18ea212e57b979e7b78896) (git@github.com:wsmoses/Tapir-LLVM.git 4ad002f2de582970c8836b0df54bd65587876fd1)"}
!2 = !{!3, !3, i64 0}
!3 = !{!"long", !4, i64 0}
!4 = !{!"omnipotent char", !5, i64 0}
!5 = !{!"Simple C/C++ TBAA"}

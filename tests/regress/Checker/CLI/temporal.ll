declare void @close_resource(i8*)
declare void @use_resource(i8*)

define void @helper(i8* %p) {
  call void @use_resource(i8* %p)
  ret void
}

define void @bad(i8* %p) {
  call void @close_resource(i8* %p)
  call void @helper(i8* %p)
  ret void
}

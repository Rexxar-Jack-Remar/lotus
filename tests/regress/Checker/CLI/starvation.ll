declare void @pthread_mutex_lock(i8*)
declare void @pthread_mutex_unlock(i8*)
declare i32 @sleep(i32)

define void @wait_helper() {
  call i32 @sleep(i32 1)
  ret void
}

define void @bad(i8* %m) {
  call void @pthread_mutex_lock(i8* %m)
  call void @wait_helper()
  call void @pthread_mutex_unlock(i8* %m)
  ret void
}

target datalayout = "e-p:64:64"
@a = global i8 0
@b = global i8 0
@slot = global i8* @a

define i8* @identity(i8* %p) {
  ret i8* %p
}

define i32 @main() {
  %before = load i8*, i8** @slot
  store i8* @b, i8** @slot
  %after = load i8*, i8** @slot
  %first = call i8* @identity(i8* @a)
  %second = call i8* @identity(i8* @b)
  ret i32 0
}

; Small cyclic/call graph for ordering-policy CLI and trace checks.
define i32 @helper(i32 %x, i1 %again) {
entry:
  br label %loop
loop:
  %y = add i32 %x, 2
  br i1 %again, label %loop, label %exit
exit:
  ret i32 %y
}

define i32 @main(i32 %a, i1 %again) {
entry:
  %r = call i32 @helper(i32 %a, i1 %again)
  ret i32 %r
}

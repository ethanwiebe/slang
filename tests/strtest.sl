; str test
(def (assert-eq x . args)
	(if (apply = x args)
		true
		(do
			(print x '!= args)
			(assert false)
		)
	)
)

(def test "this is a test!")

(assert-eq 
	'("this" "is" "a" "test!")
	(str-split " " test)
)

(assert-eq
	test
	(str-join " " (str-split " " test))
)

(assert-eq
	"this  is  a  test!"
	(str-join "  " (str-split " " test))
)

(assert-eq 
	'("" "his is a " "es" "!")
	(str-split "t" test)
)

(assert-eq 
	'("this is a test" "")
	(str-split "!" test)
)

(assert-eq 
	'("this is a test!")
	(str-split "$" test)
	(str-split "$" "@" test)
)

(def test2 "tokens with spaces\n and newlines\n\tand tabs!")

(output "str passed\n")

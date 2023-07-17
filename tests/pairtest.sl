; pair with two equal pointers
(def testref (& () (do
	(let ((a (pair 1 2)) (sec (pair a a)))
			(print sec)
			(assert (= (L sec) (R sec)))
			(assert (is (L sec) (R sec)))
			(setL! (L sec) 5)
			(assert (= (L (R sec)) 5))
	)
)))

(testref)
(testref)

; testing setL
(def testquote (& () (do
	(let (
		(x '((1 2) (3 4)))
		(y (L x))
		)
		
		(setL! (L x) ())
		(assert (= (L (L x)) ()))
		(assert (= (L y) ()))
	)
)))

(testquote)
(testquote)

(def x '(1 2))
(assert (is x x))
(def y (pair 1 2))
(assert (is y y))
(assert !(is x y))

(print 'passed)

; pair with two equal pointers
(def testref (& ()
	(let ((a (pair 1 2)) (sec (pair a a)))
			(print sec)
			(assert (= (L a) 1))
			(assert (= (L sec) (R sec)))
			(assert (is (L sec) (R sec)))
			(setL! (L sec) 5)
			(assert (= (L (R sec)) 5))
	)
))

; testing setL
(def testquote (& ()
	(let (
		(x '((1 2) (3 4)))
		(y (L x))
		)
		
		(print x)
		(setL! (L x) 12)
		(assert (= (L (L x)) 12))
		(assert (= (L y) 12))
	)
))

(testref)
(testquote)
(testref)
(testquote)

(def x '(1 2))
(assert (is x x))
(def y (pair 1 2))
(assert (is y y))
(assert !(is x y))

; testing value types
(def v1 20)
(def v2 v1)
(set! v1 21)
(assert (= v2 20))
(assert (= v1 21))

(print 'passed)

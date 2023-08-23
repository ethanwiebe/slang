; pair with two equal pointers
(def testref (& ()
	(let ((a (pair 1 2)) (sec (pair a a)))
			(assert (= (L a) 1))
			(assert (= (L sec) (R sec)))
			(assert (is (L sec) (R sec)))
			(set-L! (L sec) 5)
			(assert (= (L (R sec)) 5))
	)
))

; testing setL
(def testquote (& ()
	(let (
		(x (list (list 1 2) (list 3 4)))
		(y (L x))
		)
		
		(set-L! (L x) 12)
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

(assert (= '((1 2) 3 4) (apply pair '((1 2) (3 4)))))
(assert (= '((1 2) 3 4) (apply pair '((1 2) (3 4)))))
(assert (= '(a . b) (apply pair '(a b))))
(assert (= '(a . b) (apply pair 'a '(b))))
(assert (= '((1 . 3) (2 . 4)) (map pair (list 1 2) (list 3 4))))
(assert (= '(((1 2) 3 4)) (map pair '((1 2)) '((3 4)))))
(assert (= '((a . d) (b . e) (c . f)) (map pair '(a b c) '(d e f))))

(def ptest (parse "'(1 2 (a b) 4 5)"))

(output "pair passed\n")

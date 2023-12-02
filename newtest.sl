(def (assert-eq x y)
	(if (= x y)
		true
		(do
			(print x '!= y)
			(assert false)
		)
	)
)

(def f (& (x) (+ x 1)))
(f 3)

(def (h x y) (+ x y))


(def g (& (x y)
	(if x
		(g (- x 1) (+ y 1))
		y
	)
))


(def (mul-sub x y z)
	(if x
		(mul-sub (- x 1) y (+ y z))
		z
	)
)
(def mul (do (& (x y) (mul-sub x y 0))))

(print (mul 2 30))
;(set! mul (& () 3))

(def (vatest first . args) args)

(g 5 0)
(h 3 100)
(assert-eq 50000 (mul 1000 50))
(vatest 1 2 3 4 5)
(print "hello world" "abcd")

(def (ltest x)
	(let ((c 1) (d x))
		(set! c (+ c 1))
		(+ c d x)
	)
)

(ltest 10)
(def (make-counter)
	(let ((count 0))
		(& () 
			(set! count (++ count))
			count
		)
	)
)

(assert (pure? make-counter))

(let loop ((i 10))
	i
	(if i
		(loop (-- i))
		i
	)
)

(def (usr-defined . args)
	(+ 2 3)
)

(def movable-func ())

(set! movable-func (& () 3))
(assert-eq 3 (movable-func))
(set! movable-func (& () 4))
(assert-eq 4 (movable-func))

(def (retnull) +)

(def (replus . args) (apply + args))

(print ((retnull) 1 2))

(def (smallfunc) (++ 3) (++ 2))
(print (smallfunc))
(def c1 (make-counter))
(assert-eq (c1) 1)
(assert-eq (c1) 2)
(assert-eq (c1) 3)
(def c2 (make-counter))
(assert-eq (c2) 1)
(assert-eq (c2) 2)
(assert-eq (c1) 4)
(assert !(pure? (c1)))
(print 'test)
(print (replus 1 2 3 4 5 6))
(print 'before)

(assert (= '(12 15 18)
			(map + '(1 2 3) '(4 5 6) '(7 8 9))
))

(let ()
	(try (set! abc 3))
)

(def (map-f a) (* a a))

(map map-f '(1 2 3))

(def val (map apply '(+ * -) '((1 2 3) (4 5 6) (7 8 9))))
(assert-eq val '(6 120 -10))

(assert-eq 
	`(1 `,(+ 1 ,(+ 2 3)) 4)
	'(1 `,(+ 1 5) 4)
)

(print (let ((a 3) (b (++ a)))
	b
))

(assert-eq 70 
	(let ((x 2) (y 3))
		(let ((x 7) (z (+ x y)))
			(* z x)
		)
	)
)

;-(def prog '(do
	(def test 3)
	(+ test 6)
))

(assert (= 9 (eval prog)))
(print test)-;

(letrec
	((even? (& (n)
		(if !n
			true
			(odd? (-- n))
		)
	))
	(odd? (& (n)
		(if !n
			false
			(even? (-- n))
		)
	)))
	(even? 88)
)

(def lettestf
	(let ([x 13] [y (do (set! x 10) (++ x))])
		(& ()
			(* 2 y)
		)
	)
)

(assert-eq (lettestf) 22)

(def letrectestf
	(letrec ([a 2] [b (++ a)])
		(& ()
			(* b 2)
		)
	)
)

(print (letrectestf))

(assert-eq
	(letrec ([a 2] [b (++ a)])
		b
	)
	3
)

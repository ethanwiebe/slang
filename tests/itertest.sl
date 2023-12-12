; iter test
(def (mul x y) (* x y))

(def (assert-eq x . args)
	(if (apply = x args)
		true
		(do
			(print x '!= args)
			(assert false)
		)
	)
)

(assert-eq 
	720
	(apply * '(1 2 3 4 5 6))
	(fold * 1 '(1 2 3 4 5 6))
	(fold '* 1 '(1 2 3 4 5 6))
	(fold mul 1 '(1 2 3 4 5 6))
)

(def (even? x) !(% x 2))

(assert-eq
	'(2 4 6)
	(filter even? '(1 2 3 4 5 6))
	(filter even? '(1 2 3 4 5 6 7))
	(filter even? '(2 3 4 5 6))
	(filter even? '(2 4 6))
	(filter even? '(1 2 43 3 4 5 6 7))
	(filter int? '(1.0 2 3.0 4 5.0 5.5 6.0 6))
)

(assert-eq
	'(4 3 2 1)
	(fold (& (x y) (pair y x)) () '(1 2 3 4))
	(map -- '(5 4 3 2))
	(map '-- '(5 4 3 2))
	(map - '(10 3 4 2) '(6 0 2 1))
	(map '- '(10 3 4 2) '(6 0 2 1))
	(map (& (x) (fold '+ 0 x)) '((1 1 1 1) (2 -1 1 1) (0 1 0 1) (1)))
	(map (& (x) (fold + 0 x)) '((1 1 1 1) (2 -1 1 1) (0 1 0 1) (1)))
	(filter (& (x) (< 0 x 5)) '(8 7 6 5 4 3 2 1 0 -1))
)

(def s 0)

(foreach (& (x) (set! s (++ s))) '(() () () ()))
(assert-eq s 4)

(output "iter passed\n")

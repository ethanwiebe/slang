(+ 12 (+ 1 1))
(def abc 13)
(+ abc 1)
12
(def func 
	(& (y) (do 
					(def x 1)
					(+ x y))))





(def eq (& (x y)
				(if (- x y) false true)))

(def fact-sub (& (z curr)
					(if (eq z 1) curr (fact-sub (- z 1) (* curr z)))))

(def fact (& (x) (fact-sub x 1)))

(+ 1 2)

(def nest 
	(& (x) 
		(& (y)
			(* x y)
		)
	)
)

(def counter
	(& (x)
		(do
			(def countVal x)
			(& () 
				(set! countVal (+ countVal 1))
			)
		)
	)
)

(def mul-by-3 (nest 3))
(mul-by-3 5)

(def c1 (counter 3))
(def c2 (counter 8))

(def test (& (x) (if (eq x 0) 0 (test (- x 1)))))

(def a (/ 9 2))

(c1)
(c1)
(print (c1))
(print (c1))
(fact fact)

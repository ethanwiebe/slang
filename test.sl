(+ 12 (+ 1 1))
(def abc 13)
(+ abc 1)
12
(def func 
	(lambda (y) (do 
					(def x 1)
					(+ x y))))




(def x 1000)
(func 1)
x

(def eq (lambda (x y)
				(if (- x y) 0 1)))

(def fact-sub (lambda (z curr)
					(if (eq z 1) curr (fact-sub (- z 1) (* curr z)))))

(def fact (lambda (x) (fact-sub x 1)))

(+ 1 2)
(fact 5)
(fact 10)

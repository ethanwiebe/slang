(def glob 3)

(def counter
	(lambda (x)
		(do
			(lambda () 
				(set! x (+ x 1))
			)
		)
	)
)

(def c1
	(counter 1))
(def c2 (counter 10))

(c1)
(c1)
(c2)
(c1)

''(+ 2 3)
''abc

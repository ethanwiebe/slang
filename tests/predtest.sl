(def errored? (& (expr) 
	(empty? (try (eval expr)))
))

(assert (null? ()))
(assert !(null? 2))
(assert (pair? '(1 2)))
(assert (pair? (pair () ())))
(assert (num? 2))
(assert (num? 2.1))
(assert (num? (+ 2.1 1.3)))
(assert !(int? 2.1))
(assert !(int? 2.0))
(assert !(real? 2))
(assert (real? .2))
(assert (str? "hello"))
(assert (str? ""))
(assert !(null? ""))
; empty string has false value
(assert !"")
; zero has false value
(assert !0)
(assert !!2)
(assert !!-1)
; null has false value
(assert !())
(assert (null? ((& () ()))))
(assert (proc? (& () ())))
(assert (vec? (vec 123 4 'hello)))
(assert (vec? (vec-alloc 100)))
(def t 3)
(assert (bound? 't))
(assert (bound? 'bound?))
(assert !(bound? 'abc))
(assert (maybe? (try)))
(assert (maybe? (try (+ 3 4))))
(assert (maybe? (try (+ 3 'abc))))
(assert !(try))
(assert !!(try (+ 3 4)))
(assert !(try (+ 3 'def)))
(assert (empty? (try)))
(assert !(empty? (try 3)))
(assert (empty? (try (+ 3 'hello))))
(assert !(errored? '(+ 3 4)))
(assert (errored? '(+ 3 'hello)))
(assert !(errored? '(set! t ())))
(assert !(eof? ()))
(assert (null? t))
(assert (> 3 2))
(assert !(> 3 3))
(assert (>= 3 3))
(assert (> 3 2.99999))

(output "predicate passed\n")

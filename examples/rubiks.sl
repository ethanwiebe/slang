; rubiks cube

(def (reset-bg)
	(output "\e[0m")
)

(def (set-bg-col col)
	(case col
		((red) (output "\e[41m"))
		((blue) (output "\e[44m"))
		((green) (output "\e[42m"))
		((yellow) (output "\e[43m"))
		((white) (output "\e[47m"))
		((orange) (output "\e[48;5;208m"))
		
		(else assert false)
	)
)

;-(def (list-get l index)
	(let loop ((i 0) (curr l))
		(if (= i index)
			(L curr)
			(loop (++ i) (R curr))
		)
	)
)

(def (list-set! l index item)
	(let loop ((i 0) (curr l))
		(if (= i index)
			(set-L! curr item)
			(loop (++ i) (R curr))
		)
	)
)-;

(def (list-swap! l i1 i2)
	(let ((tmp (list-get l i1)))
		(list-set! l i1 (list-get l i2))
		(list-set! l i2 tmp)
		l
	)
)

;-(def (list-swap-sub l lIt elem i2)
	(let loop ((it lIt) (j2 i2))
		(if (= 0 j2)
			(do
				(set-L! l (L it))
				(set-L! it elem)
				()
			)
			(loop (R it) (-- j2))
		)
	)
)

(def (list-swap! l i1 i2)
	(let ((ind1 (min i1 i2)) (ind2 (max i1 i2)))
		(let loop ((j1 ind1) (j2 ind2) (lIt l))
			(if (= 0 j1)
				(list-swap-sub lIt lIt (L lIt) j2)
				(loop (-- j1) (-- j2) (R lIt))
			)
		)
	
	)
)-;

(def (list-rev l)
	(let loop ((newL ()) (oldL l))
		(if (null? oldL)
			newL
			(loop (pair (L oldL) newL) (R oldL))
		)
	)
)

(def (apply-perm! l perm)
	(cond
		((< (len perm) 2) l)
		((= (len perm) 2) (apply list-swap! l perm))
		(else
			(let loop ((currPerm (list-rev perm)) (p (L currPerm)))
				(if (null? (R currPerm))
					; last elem
					l
					(do
						; swap two
						(list-swap! l p (L (R currPerm)))
						(loop (R currPerm) (L (R currPerm)))
					)
				)
			)
		)
	)
)

(def (apply-perms! l perms)
	(if (null? perms)
		l
		(do 
			(apply-perm! l (L perms))
			(apply-perms! l (R perms))
		)
	)
)

(def (default-corners)
	(list 
		; 0
		'yellow 'red 'blue
		'yellow 'blue 'orange
		'yellow 'orange 'green
		'yellow 'green 'red
		
		; 12
		'white 'red 'green
		'white 'green 'orange
		'white 'orange 'blue
		'white 'blue 'red
	)
)

(def (default-edges)
	(list
		; 0
		'yellow 'blue
		'yellow 'orange
		'yellow 'green
		'yellow 'red
		
		; 8
		'red 'blue
		'blue 'orange
		'orange 'green
		'green 'red
		
		; 16
		'white 'green
		'white 'orange
		'white 'blue
		'white 'red
	)
)

(def (apply-B cube)
	(apply-perms! (L cube) '((0 23 18 5) (1 21 19 3) (2 22 20 4)))
	(apply-perms! (R cube) '((0 8 20 11) (1 9 21 10)))
)

(def (apply-B' cube)
	(apply-perms! (L cube) '((5 18 23 0) (3 19 21 1) (4 20 22 2)))
	(apply-perms! (R cube) '((11 20 8 0) (10 21 9 1)))
)

(def (apply-F cube)
	(apply-perms! (L cube) '((6 17 12 11) (7 15 13 9) (8 16 14 10)))
	(apply-perms! (R cube) '((4 12 16 15) (5 13 17 14)))
)

(def (apply-F' cube)
	(apply-perms! (L cube) '((11 12 17 6) (9 13 15 7) (10 14 16 8)))
	(apply-perms! (R cube) '((15 16 12 4) (14 17 13 5)))
)

(def (apply-L cube)
	(apply-perms! (L cube) '((18 16 6 4) (19 17 7 5) (20 15 8 3)))
	(apply-perms! (R cube) '((10 18 13 2) (11 19 12 3)))
)

(def (apply-L' cube)
	(apply-perms! (L cube) '((4 6 16 18) (5 7 17 19) (3 8 15 20)))
	(apply-perms! (R cube) '((2 13 18 10) (3 12 19 11)))
)

(def (apply-R cube)
	(apply-perms! (L cube) '((0 10 12 22) (1 11 13 23) (2 9 14 21)))
	(apply-perms! (R cube) '((6 14 22 9) (7 15 23 8)))
)

(def (apply-R' cube)
	(apply-perms! (L cube) '((22 12 10 0) (23 13 11 1) (21 14 9 2)))
	(apply-perms! (R cube) '((9 22 14 6) (8 23 15 7)))
)

(def (apply-U cube)
	(apply-perms! (L cube) '((12 15 18 21) (13 16 19 22) (14 17 20 23)))
	(apply-perms! (R cube) '((16 18 20 22) (17 19 21 23)))
)

(def (apply-U' cube)
	(apply-perms! (L cube) '((21 18 15 12) (22 19 16 13) (23 20 17 14)))
	(apply-perms! (R cube) '((22 20 18 16) (23 21 19 17)))
)

(def (apply-D cube)
	(apply-perms! (L cube) '((0 3 6 9) (1 4 7 10) (2 5 8 11)))
	(apply-perms! (R cube) '((0 2 4 6) (1 3 5 7)))
)

(def (apply-D' cube)
	(apply-perms! (L cube) '((9 6 3 0) (10 7 4 1) (11 8 5 2)))
	(apply-perms! (R cube) '((6 4 2 0) (7 5 3 1)))
)


(def (make-cube)
	(pair (default-corners) (default-edges))
)

(let ((test-cube (make-cube)))
	(apply-U test-cube)
	(apply-D test-cube)
	(apply-L test-cube)
	(apply-R test-cube)
	(apply-F test-cube)
	(apply-B test-cube)
	(apply-B' test-cube)
	(apply-F' test-cube)
	(apply-R' test-cube)
	(apply-L' test-cube)
	(apply-D' test-cube)
	(apply-U' test-cube)
	(assert (= test-cube (make-cube)))
)

(def (invert-algorithm alg)
	(let loop ((newAlg ()) (oldAlg alg))
		(if (null? oldAlg)
			newAlg
			(loop (pair 
				(case (L oldAlg)
					((U2 D2 L2 R2 F2 B2) (L oldAlg))
					((U) 'U')
					((U') 'U)
					((D) 'D')
					((D') 'D)
					((R) 'R')
					((R') 'R)
					((L) 'L')
					((L') 'L)
					((F) 'F')
					((F') 'F)
					((B) 'B')
					((B') 'B)
					(else (assert false))
				)
				newAlg)
				(R oldAlg)
			)
		)
	)
)

(def (apply-algorithm! cube alg)
	(if (null? alg)
		cube
		(do 
			(case (L alg)
				((U) (apply-U cube))
				((U2) (apply-U cube) (apply-U cube))
				((U') (apply-U' cube))
				
				((R) (apply-R cube))
				((R2) (apply-R cube) (apply-R cube))
				((R') (apply-R' cube))
				
				((D) (apply-D cube))
				((D2) (apply-D cube) (apply-D cube))
				((D') (apply-D' cube))
				
				((L) (apply-L cube))
				((L2) (apply-L cube) (apply-L cube))
				((L') (apply-L' cube))
				
				((F) (apply-F cube))
				((F2) (apply-F cube) (apply-F cube))
				((F') (apply-F' cube))
				
				((B) (apply-B cube))
				((B2) (apply-B cube) (apply-B cube))
				((B') (apply-B' cube))
				
				(else (assert false))
			)
			(apply-algorithm! cube (R alg))
		)
	)
)

(def (display-cube-piece piece)
	(set-bg-col piece)
	(output "  ")
	(reset-bg)
)

(def (display-cube cube)
	(let ((corners (L cube)) (edges (R cube)))
		; white face
		(output "      ")
		(display-cube-piece (list-get corners 18))
		(display-cube-piece (list-get edges 20))
		(display-cube-piece (list-get corners 21))
		(output "\n")
		
		(output "      ")
		(display-cube-piece (list-get edges 18))
		(display-cube-piece 'white)
		(display-cube-piece (list-get edges 22))
		(output "\n")
		
		(output "      ")
		(display-cube-piece (list-get corners 15))
		(display-cube-piece (list-get edges 16))
		(display-cube-piece (list-get corners 12))
		(output "\n")
		
		; orange top
		(display-cube-piece (list-get corners 19))
		(display-cube-piece (list-get edges 19))
		(display-cube-piece (list-get corners 17))
		
		; green top
		(display-cube-piece (list-get corners 16))
		(display-cube-piece (list-get edges 17))
		(display-cube-piece (list-get corners 14))
		
		; red top
		(display-cube-piece (list-get corners 13))
		(display-cube-piece (list-get edges 23))
		(display-cube-piece (list-get corners 23))
		
		; blue top
		(display-cube-piece (list-get corners 22))
		(display-cube-piece (list-get edges 21))
		(display-cube-piece (list-get corners 20))
		(output "\n")
		
		; orange middle
		(display-cube-piece (list-get edges 11))
		(display-cube-piece 'orange)
		(display-cube-piece (list-get edges 12))
		
		; green middle
		(display-cube-piece (list-get edges 13))
		(display-cube-piece 'green)
		(display-cube-piece (list-get edges 14))
		
		; red middle
		(display-cube-piece (list-get edges 15))
		(display-cube-piece 'red)
		(display-cube-piece (list-get edges 8))
		
		; blue middle
		(display-cube-piece (list-get edges 9))
		(display-cube-piece 'blue)
		(display-cube-piece (list-get edges 10))
		(output "\n")
		
		; orange bottom
		(display-cube-piece (list-get corners 5))
		(display-cube-piece (list-get edges 3))
		(display-cube-piece (list-get corners 7))
		
		; green bottom
		(display-cube-piece (list-get corners 8))
		(display-cube-piece (list-get edges 5))
		(display-cube-piece (list-get corners 10))
		
		; red bottom
		(display-cube-piece (list-get corners 11))
		(display-cube-piece (list-get edges 7))
		(display-cube-piece (list-get corners 1))
		
		; blue bottom
		(display-cube-piece (list-get corners 2))
		(display-cube-piece (list-get edges 1))
		(display-cube-piece (list-get corners 4))
		(output "\n")
		
		; yellow face
		(output "      ")
		(display-cube-piece (list-get corners 6))
		(display-cube-piece (list-get edges 4))
		(display-cube-piece (list-get corners 9))
		(output "\n")
		
		(output "      ")
		(display-cube-piece (list-get edges 2))
		(display-cube-piece 'yellow)
		(display-cube-piece (list-get edges 6))
		(output "\n")
		
		(output "      ")
		(display-cube-piece (list-get corners 3))
		(display-cube-piece (list-get edges 0))
		(display-cube-piece (list-get corners 0))
		(output "\n")
	)
)

(def (test-alg-cycle algo)
	(let ((c (make-cube)) (blank-c (make-cube)))
		(let loop ((curr (apply-algorithm! c algo)) (i 1))
			(if (= curr blank-c)
				i
				(do
					(apply-algorithm! curr algo)
					;(output i "\n")
					;(display-cube c)
					(loop curr (++ i))
				)
			)
		)
	)
)

(def c (make-cube))
(def test-alg '(F2 L2 D2 U2 B L2 D2 F U2 F' U2 R' D L D B U L D2 L' U))
(def t-perm '(R U R' U' R' F R2 U' R' U' R U R' F'))
(def long-alg '(R U2 D' B D'))
;(apply-algorithm! c test-alg)
;(apply-algorithm! c (invert-algorithm test-alg))

(def (output-alg alg)
	(cond
		((null? (R alg)) (output (L alg)))
		(else 
			(output (L alg) " ")
			(output-alg (R alg))
		)
	)
)
;(output-alg test-alg)
;(output "\n")
;(display-cube c)
(import (slang time))

(def start-t (perf-time))
(print start-t)
(test-alg-cycle long-alg)
(print (* 1000 (- (perf-time) start-t)))
(print (perf-time))


#include "codecrypt.h"

using namespace ccr;
using namespace ccr::mce_qd;

#include "decoding.h"
#include "fwht.h"

#include <set>

static uint sample_from_u (gf2m&fld, prng&rng, std::set<uint>&used)
{
	uint x;
	for (;;) {
		x = rng.random (fld.n);
		if (used.count (x) ) continue;
		used.insert (x);
		return x;
	}
}

static uint choose_random (uint limit, prng&rng, std::set<uint>used)
{
	if (used.size() >= limit - 1) return 0; //die
	for (;;) {
		uint a = 1 + rng.random (limit - 1);
		if (used.count (a) ) continue;
		used.insert (a);
		return a;
	}
}

int mce_qd::generate (pubkey&pub, privkey&priv, prng&rng,
                      uint m, uint T, uint block_discard)
{
	priv.fld.create (m);
	priv.T = T;
	uint t = 1 << T;

	std::cout << "generate" << std::endl;
	//convenience
	gf2m&fld = priv.fld;
	std::vector<uint>&Hsig = priv.Hsig;
	std::vector<uint>&essence = priv.essence;
	std::vector<uint>&support = priv.support;
	polynomial&g = priv.g;

	//prepare for data
	Hsig.resize (fld.n / 2);
	support.resize (fld.n / 2);
	essence.resize (m);
	//note that q=2^m, algo. n=q/2, log n = m-1

	//retry generating until goppa code is produced.
	for (;;) {

		std::set<uint> used;
		used.clear();

		std::cout << "attempt..." << std::endl;
		//first off, compute the H signature

		Hsig[0] = choose_random (fld.n, rng, used);
		essence[m - 1] = fld.inv (Hsig[0]);
		//essence[m-1] is now used as precomputed 1/h_0

		for (uint s = 0; s < m - 1; ++s) {
			uint i = 1 << s; //i = 2^s

			Hsig[i] = choose_random (fld.n, rng, used);
			essence[s] = fld.add (essence[m - 1], fld.inv (Hsig[i]) );
			used.insert (fld.inv (essence[s]) );

			for (uint j = 1; j < i; ++j) {
				Hsig[i + j] = fld.inv
				              (fld.add
				               (fld.inv (Hsig[i]),
				                fld.add (
				                    fld.inv (Hsig[j]),
				                    essence[m - 1]
				                ) ) );
				used.insert (Hsig[i + j]);
				used.insert (fld.inv
				             (fld.add
				              (fld.inv (Hsig[i + j]),
				               essence[m - 1]) ) );
			}
		}

		//from now on, we fix 'omega' from the paper to zero.

		std::cout << "goppa..." << std::endl;
		//assemble goppa polynomial.
		g.clear();
		g.resize (1, 1); //g(x)=1 so we can multiply it
		polynomial tmp;
		tmp.resize (2, 1); //tmp(x)=x-1
		for (uint i = 0; i < t; ++i) {
			//tmp(x)=x-z=x-(1/h_i)
			tmp[0] = fld.inv (Hsig[i]);
			g.mult (tmp, fld);
			std::cout << "computing g... " << g;
		}

		std::cout << "Goppa poly " << g;

		std::cout << "support..." << std::endl;
		//compute the support, retry if it has two equal elements.
		used.clear();
		bool consistent = true;
		for (uint i = 0; i < fld.n / 2; ++i) {
			support[i] = fld.add (
			                 fld.inv (Hsig[i]),
			                 essence[m - 1]);

			if (used.count (support[i]) ) {
				consistent = false;
				break;
			}

			if (g.eval (support[i], fld) == 0) {
				std::cout << "support zero!" << std::endl;
				consistent = false;
				break;
			}

			std::cout << "support at " << i << ": " << support[i] << std::endl;

			used.insert (support[i]);
		}
		if (!consistent) continue; //retry

		std::cout << "blocks..." << std::endl;
		//now the blocks.
		uint block_size = 1 << T,
		     h_block_count = (fld.n / 2) / block_size;
		uint& block_count = priv.block_count;
		block_count = h_block_count - block_discard;

		//assemble blocks to bl
		std::vector<polynomial> bl, blp;
		bl.resize (h_block_count);
		for (uint i = 0; i < h_block_count; ++i) {
			bl[i].resize (block_size);
			for (uint j = 0; j < block_size; ++j)
				bl[i][j] = Hsig[i * block_size + j];
		}

		std::cout << "permuting blocks..." << std::endl;
		//permute them
		priv.block_perm.generate_random (h_block_count, rng);
		priv.block_perm.permute (bl, blp);

		std::cout << "discarding blocks..." << std::endl;
		//discard blocks
		blp.resize (block_count);

		std::cout << "permuting dyadic blocks..." << std::endl;
		//permute individual blocks
		priv.block_perms.resize (block_count);
		bl.resize (blp.size() );
		for (uint i = 0; i < block_count; ++i) {
			priv.block_perms[i] = rng.random (block_size);
			permutation::permute_dyadic (priv.block_perms[i],
			                             blp[i], bl[i]);
		}

		//try several permutations to construct G
		uint attempts = 0;
		for (attempts = 0; attempts < block_count; ++attempts) {
			std::cout << "generating G..." << std::endl;

			/*
			 * try computing the redundancy block of G
			 *
			 * First, co-trace H, then compute G in form
			 *
			 * G^T = [I | X]
			 *
			 * Since H*G^T = [L | R] * [I | X] = L + R*X = 0
			 * we have the solution: X = R^1 * L
			 * (thanks to Rafael Misoczki)
			 *
			 * Inversion is done the quasi-dyadic way:
			 *
			 * - because for QD matrix m=delta(h) the product
			 *   m*m = sum(h) * I, binary QD matrix m is either
			 *   inversion of itself (m*m=I) or isn't invertible
			 *   and m*m=0. sum(h), the "count of ones in QD
			 *   signature mod 2", easily determines the result.
			 *
			 * - Using blockwise invertions/multiplications,
			 *   gaussian elimination needed to invert the right
			 *   square of H can be performed in O(m^2*block_count)
			 *   matrix operations. Matrix operations are either
			 *   addition (O(t) on QDs), multiplication(O(t log t)
			 *   on QDs) or inversion (O(t), as shown above).
			 *   Whole proces is therefore quite fast.
			 *
			 * Gaussian elimination on the QD signature should
			 * result in something like this: (for m=3, t=4)
			 *
			 *   1010 0101 1001 1000 0000 0000
			 *   0101 1100 1110 0000 1000 0000
			 *   0111 1110 0100 0000 0000 1000
			 */

			priv.hperm.generate_random (block_count, rng);

			std::vector<std::vector<bvector> > hblocks;
			bvector tmp;
			bool failed;
			uint i, j, k, l;

			//prepare blocks of h
			hblocks.resize (block_count);
			for (i = 0; i < block_count; ++i)
				hblocks[i].resize (fld.m);

			//fill them from Hsig
			for (i = 0; i < block_count; ++i) {
				tmp.from_poly_cotrace (bl[priv.hperm[i]], fld);
				for (j = 0; j < fld.m; ++j)
					tmp.get_block (j * block_size,
					               block_size,
					               hblocks[i][j]);
			}

			/* now do a modified gaussian elimination on hblocks */
			failed = false;
			tmp.resize (block_size);
			for (i = 0; i < fld.m; ++i) { //gauss step
				//first, find a nonsingular matrix in the column
				for (j = i; j < fld.m; ++j)
					if (hblocks[block_count - fld.m + i][j]
					    .hamming_weight() % 2) break;
				if (j >= fld.m) { //none found, break;
					failed = true;
					break;
				}

				//bring it to correct position (swap it to i-th row)
				if (j > i) for (k = 0; k < block_count; ++k)
						hblocks[k][i].swap
						(hblocks[k][j]);

				//now normalize the row
				for (j = i; j < fld.m; ++j) {
					uint l = hblocks
					         [block_count - fld.m + i]
					         [j].hamming_weight();
					if (l == 0) continue; //zero is just okay :]
					if (! (l % 2) ) //singular, make it regular by adding the i-th row
						for (k = 0;
						     k < block_count;
						     ++k)
							hblocks[k][j].add
							(hblocks[k][i]);

					//now a matrix is regular, we can easily make it I
					for (k = 0; k < block_count; ++k) {
						fwht_dyadic_multiply
						(hblocks[block_count - fld.m + i][j],
						 hblocks[k][j], tmp);
						hblocks[k][j] = tmp;
					}

					//and zero the column below diagonal
					if (j > i) for (k = 0; k < block_count; ++k)
							hblocks[k][j].add
							(hblocks[k][i]);
				}
			}

			if (failed) continue;

			for (i = 0; i < fld.m; ++i) { //jordan step
				//normalize diagonal (it's already nonsingular)
				for (k = 0; k < block_count; ++k) {
					fwht_dyadic_multiply
					(hblocks[block_count - i - 1][fld.m - i - 1],
					 hblocks[k][fld.m - i - 1], tmp);
					hblocks[k][fld.m - i - 1] = tmp;
				}

				//now make zeroes above
				for (j = i + 1; j < fld.m; ++j) {
					l = hblocks[block_count - i - 1]
					    [fld.m - j - 1].hamming_weight();
					if (l == 0) continue; //already zero
					if (! (l % 2) ) { //nonsingular, fix it by adding diagonal
						for (k = 0; k < block_count; ++k)
							hblocks[k][fld.m - j - 1].add
							(hblocks[k][fld.m - i - 1]);
					}
					for (k = 0; k < block_count; ++k) {
						fwht_dyadic_multiply
						(hblocks[block_count - i - 1]
						 [fld.m - j - 1],
						 hblocks[k][fld.m - j - 1], tmp);
						hblocks[k][fld.m - j - 1] = tmp;
					}
					//I+I=0
					for (k = 0; k < block_count; ++k)
						hblocks[k][fld.m - j - 1].add
						(hblocks[k][fld.m - i - 1]);
				}
			}

			if (failed) continue;

			pub.qd_sigs.resize (block_count - fld.m);
			for (uint i = 0; i < block_count - fld.m; ++i) {
				pub.qd_sigs[i].resize (block_size * fld.m);
				for (uint j = 0; j < fld.m; ++j)
					pub.qd_sigs[i].set_block
					(hblocks[i][j], block_size * j);
			}

			break;
		}

		if (attempts == block_count) //generating G failed, retry all
			continue;

		//finish the pubkey
		pub.T = T;

		return 0;
	}
}

int privkey::prepare()
{
	uint s, i, j;
	std::cout << "prepare" << std::endl;
	//compute H signature from essence
	Hsig.resize (fld.n / 2);
	Hsig[0] = fld.inv (essence[fld.m - 1]);
	for (s = 0; s < fld.m - 1; ++s) {
		i = 1 << s; //i = 2^s

		Hsig[i] = fld.inv (fld.add (essence[s], essence[fld.m - 1]) );

		for (j = 1; j < i; ++j)
			Hsig[i + j] = fld.inv
			              (fld.add
			               (fld.inv (Hsig[i]),
			                fld.add (
			                    fld.inv (Hsig[j]),
			                    essence[fld.m - 1]
			                ) ) );
	}

	//compute the support
	support.resize (fld.n / 2);
	std::set<uint> used_support;
	for (i = 0; i < fld.n / 2; ++i) {
		support[i] = fld.add
		             (fld.inv (Hsig[i]),
		              essence[fld.m - 1]);

		//support consistency check
		if (used_support.count (support[i]) )
			return 1;
		used_support.insert (support[i]);
	}

	//prepare permuted Hsig (so that it can be applied directly)
	//and prepare reverse support position lookup data
	uint block_size = 1 << T;
	uint pos, blk_perm;
	polynomial tmp_blk, tmp_pblk;
	bvector cotrace, tmp_block;
	std::vector<uint> sbl1, sbl2, permuted_support;

	tmp_blk.resize (block_size);
	tmp_pblk.resize (block_size);

	Hc.resize (fld.m);
	for (i = 0; i < fld.m; ++i) {
		Hc[i].resize (block_count);
	}

	permuted_support.resize (block_size * block_count);
	sbl1.resize (block_size);
	sbl2.resize (block_size);

	//go through all the blocks of original H and convert them if they
	//aren't discarded.
	for (i = 0; i < (fld.n / 2) / block_size; ++i) {
		pos = block_perm[i];
		if (pos >= block_count) continue; //was discarded
		blk_perm = block_perms[pos];
		pos = hperm[pos];

		//permute i-th block of H to pos-th block of Hc,
		//also move the support.
		for (j = 0; j < block_size; ++j) {
			tmp_blk[j] = Hsig[j + i * block_size];
			sbl1[j] = support[j + i * block_size];
		}
		permutation::permute_dyadic (blk_perm, tmp_blk, tmp_pblk);
		permutation::permute_dyadic (blk_perm, sbl1, sbl2);

		//store support to permuted support
		for (j = 0; j < block_size; ++j)
			permuted_support[j + pos * block_size] = sbl2[j];

		//cotrace permuted H block to Hc
		cotrace.from_poly_cotrace (tmp_pblk, fld);

		for (j = 0; j < fld.m; ++j)
			cotrace.get_block (j * block_size, block_size, Hc[j][pos]);
	}

	//convert the support result to actual lookup
	support_pos.clear();
	//fld.n in support lookup means that it isn't there (we don't have -1)
	support_pos.resize (fld.n, fld.n);
	for (i = 0; i < block_size * block_count; ++i)
		support_pos[permuted_support[i]] = i;

	for (i = 0; i < support_pos.size(); ++i) {
		std::cout << "support " << i << " has position " << support_pos[i] << std::endl;
	}

	//goppa polynomial
	g.clear();
	g.resize (1, 1);
	polynomial tmp;
	tmp.resize (2, 1);
	uint t = 1 << T;
	for (i = 0; i < t; ++i) {
		tmp[0] = fld.inv (Hsig[i]);
		g.mult (tmp, fld);
	}

	//sqInv
	g.compute_square_root_matrix (sqInv, fld);

	return 0;
}

int pubkey::encrypt (const bvector & in, bvector & out, prng & rng)
{
	uint t = 1 << T;
	bvector p, g, r, cksum;
	uint i, j, k;

	/*
	 * shortened checksum pair of G is computed blockwise accordingly to
	 * the t-sized square dyadic blocks.
	 */

	//some checks
	if (!qd_sigs.size() ) return 1;
	if (qd_sigs[0].size() % t) return 1;

	uint blocks = qd_sigs[0].size() / t;
	cksum.resize (qd_sigs[0].size(), 0);

	p.resize (t);
	g.resize (t);
	r.resize (t);

	for (i = 0; i < qd_sigs.size(); ++i) {
		//plaintext block
		in.get_block (i * t, t, p);

		for (j = 0; j < blocks; ++j) {
			//checksum block
			qd_sigs[i].get_block (j * t, t, g);

			//block result
			fwht_dyadic_multiply (p, g, r);
			//std::cout << "DYADIC MULTIPLY: " << p << g << r << "---" << std::endl;
			cksum.add_offset (r, t * j);
			//std::cout << "CKSUM NOW: " << cksum;
		}
	}

	//generate t errors
	bvector e;
	e.resize (cipher_size(), 0);
	for (uint n = t; n > 0;) {
		uint p = rng.random (e.size() );
		if (!e[p]) {
			e[p] = 1;
			--n;
		}
	}

	//compute ciphertext
	out = in;
	out.insert (out.end(), cksum.begin(), cksum.end() );
	out.add (e);

	return 0;
}

int privkey::decrypt (const bvector & in, bvector & out)
{
	if (in.size() != cipher_size() ) return 2;

	//multiply line-by-line block-by-block by H
	uint block_size = 1 << T;
	bvector synd_vec;
	bvector hp, cp, res;
	uint i, j, k;

	synd_vec.resize (block_size * fld.m);
	cp.resize (block_size);
	res.resize (block_size);

	for (i = 0; i < block_count; ++i) {
		in.get_block (i * block_size, block_size, cp);
		for (j = 0; j < fld.m; ++j) {
			fwht_dyadic_multiply (Hc[j][i], cp, res);
			synd_vec.add_offset (res, j * block_size);
		}
	}

	//decoding
	polynomial synd, loc;
	synd_vec.to_poly_cotrace (synd, fld);
	compute_error_locator (synd, fld, g, sqInv, loc);

	bvector ev;
	if (!evaluate_error_locator_trace (loc, ev, fld) )
		return 1; //couldn't decode
	//TODO evaluator should return error positions, not bvector. fix it everywhere!

	out = in;
	out.resize (plain_size() );
	//flip error positions of out.
	for (i = 0; i < ev.size(); ++i) if (ev[i]) {
			if (support_pos[i] == fld.n) return 1; //couldn't decode TODO is it true?
			if (i < plain_size() )
				out[i] = !out[i];
		}

	return 0;
}


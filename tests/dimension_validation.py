from test import TestCase, generate_random_vector
import struct
import redis.exceptions

class DimensionValidation(TestCase):
    def getname(self):
        return "Dimension Validation with Projection"

    def estimated_runtime(self):
        return 0.5

    def test(self):
        # Test scenario 1: Create a set with projection
        original_dim = 100
        reduced_dim = 50
        
        # Create the initial vector and set with projection
        vec1 = generate_random_vector(original_dim)
        vec1_bytes = struct.pack(f'{original_dim}f', *vec1)
        
        # Add first vector with projection
        result = self.redis.execute_command('VADD', self.test_key, 
                                          'REDUCE', reduced_dim,
                                          'FP32', vec1_bytes, f'{self.test_key}:item:1')
        assert result == 1, "First VADD with REDUCE should return 1"
        
        # Check VINFO returns the correct projection information
        info = self.redis.execute_command('VINFO', self.test_key)
        assert isinstance(info, dict), "VINFO should return a dictionary"
        assert 'vector-dim' in info, "VINFO should contain vector-dim"
        assert info['vector-dim'] == reduced_dim, f"Expected reduced dimension {reduced_dim}, got {info['vector-dim']}"
        assert 'proj-input-dim' in info, "VINFO should contain proj-input-dim"
        assert info['proj-input-dim'] == original_dim, f"Expected original dimension {original_dim}, got {info['proj-input-dim']}"
        assert 'proj-enabled' in info, "VINFO should contain proj-enabled"
        assert info['proj-enabled'] is True, "Projection should be enabled"
        
        # Test scenario 2: Try adding a mismatched vector - should fail
        wrong_dim = 80
        wrong_vec = generate_random_vector(wrong_dim)
        wrong_vec_bytes = struct.pack(f'{wrong_dim}f', *wrong_vec)
        
        # This should fail with dimension mismatch error
        try:
            self.redis.execute_command('VADD', self.test_key, 
                                     'REDUCE', reduced_dim, 
                                     'FP32', wrong_vec_bytes, f'{self.test_key}:item:2')
            assert False, "VADD with wrong dimension should fail"
        except redis.exceptions.ResponseError as e:
            assert "Input dimension mismatch for projection" in str(e), f"Expected dimension mismatch error, got: {e}"
            
        # Test scenario 3: Add a correctly-sized vector
        vec2 = generate_random_vector(original_dim)
        vec2_bytes = struct.pack(f'{original_dim}f', *vec2)
        
        # This should succeed
        result = self.redis.execute_command('VADD', self.test_key, 
                                          'REDUCE', reduced_dim,
                                          'FP32', vec2_bytes, f'{self.test_key}:item:3')
        assert result == 1, "VADD with correct dimensions should succeed"
        
        # Check VSIM also validates input dimensions
        wrong_query = generate_random_vector(wrong_dim)
        try:
            self.redis.execute_command('VSIM', self.test_key, 
                                     'VALUES', wrong_dim, *[str(x) for x in wrong_query], 
                                     'COUNT', 10)
            assert False, "VSIM with wrong dimension should fail"
        except redis.exceptions.ResponseError as e:
            assert "Input dimension mismatch for projection" in str(e), f"Expected dimension mismatch error in VSIM, got: {e}" 